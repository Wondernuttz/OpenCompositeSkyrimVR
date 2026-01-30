#include "stdafx.h"
#define BASE_IMPL
#include "BaseCompositor.h"
#include "BaseOverlay.h"
#include "BaseSystem.h"
#include "Compositor/compositor.h"
#include "Drivers/Backend.h"
#include "Misc/Config.h"
#include "Misc/ScopeGuard.h"
#include "convert.h"
#include "generated/static_bases.gen.h"
#include <string>
#include <Windows.h>

// PrismaUI keyboard integration (Skyrim VR)
#include "Misc/Keyboard/PrismaUI_API.h"

static PRISMA_UI_API::IVPrismaUI1* g_prisma = nullptr;
static PrismaView g_keyboardView = 0;
static bool g_prismaChecked = false;
static bool g_prismaAvailable = false;
static std::string g_prismaKeyboardResult;
static bool g_prismaKeyboardDone = false;
static bool g_prismaKeyboardActive = false;

// Store the event dispatch callback so the static JS callback can fire VREvent_KeyboardDone
// directly into the game's event queue when the user submits text.
// This is critical: Skyrim polls PollNextEvent() waiting for VREvent_KeyboardDone
// BEFORE it ever calls GetKeyboardText(). If we only fire the event inside
// GetKeyboardText(), the game deadlocks waiting for an event that never arrives.
static VRKeyboard::eventDispatch_t g_pendingKeyboardDispatch;
static uint64_t g_pendingKeyboardUserValue = 0;

// Forward declaration for the JS submit callback
static void OnPrismaKeyboardSubmit(const char* text);

static bool IsSkyrimVR() {
	static int cached = -1;
	if (cached == -1) {
		char exeName[MAX_PATH];
		GetModuleFileNameA(NULL, exeName, MAX_PATH);
		cached = (strstr(exeName, "SkyrimVR") != nullptr) ? 1 : 0;
	}
	return cached == 1;
}

// Detect if Skyrim VR has a game-pausing menu open (inventory, MCM, journal, etc.)
// Reads directly from Skyrim VR's UI singleton in memory.
// UI singleton offset: 0x1F83200 (from SkyrimVR.exe base)
// numPausesGame offset: 0x160 (within UI class)
static bool IsSkyrimMenuOpen() {
	if (!IsSkyrimVR()) return false;

	static uintptr_t moduleBase = 0;
	if (!moduleBase) {
		moduleBase = (uintptr_t)GetModuleHandleA("SkyrimVR.exe");
		if (!moduleBase) return false;
	}

	// Read the UI singleton pointer
	uintptr_t uiPtr = *(uintptr_t*)(moduleBase + 0x1F83200);
	if (!uiPtr) return false;

	// numPausesGame: counts how many open menus have the "pause game" flag.
	// If > 0, a menu like inventory, journal, MCM, crafting, etc. is open.
	uint32_t numPaused = *(uint32_t*)(uiPtr + 0x160);
	return numPaused > 0;
}

static bool InitPrismaUI() {
	if (g_prismaChecked) return g_prismaAvailable;
	g_prismaChecked = true;

	if (!IsSkyrimVR()) {
		g_prismaAvailable = false;
		return false;
	}

	auto api = PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V1);
	if (!api) {
		g_prismaAvailable = false;
		return false;
	}

	g_prisma = static_cast<PRISMA_UI_API::IVPrismaUI1*>(api);

	// Create the keyboard view (hidden initially)
	g_keyboardView = g_prisma->CreateView("oc-keyboard/index.html", nullptr);
	if (g_keyboardView) {
		g_prisma->Hide(g_keyboardView);
		g_prisma->RegisterJSListener(g_keyboardView, "onKeyboardSubmit", OnPrismaKeyboardSubmit);
		g_prismaAvailable = true;
	}

	return g_prismaAvailable;
}

static void OnPrismaKeyboardSubmit(const char* text) {
	g_prismaKeyboardResult = text ? text : "";
	g_prismaKeyboardDone = true;
	g_prismaKeyboardActive = false;

	// Hide the keyboard overlay
	if (g_prisma && g_keyboardView) {
		g_prisma->Unfocus(g_keyboardView);
		g_prisma->Hide(g_keyboardView);
	}

	// CRITICAL: Fire VREvent_KeyboardDone immediately into the game's event queue.
	// Skyrim's game loop is: ShowKeyboard() -> poll PollNextEvent() waiting for
	// VREvent_KeyboardDone -> THEN call GetKeyboardText().
	// If we wait until GetKeyboardText() to fire the event, we deadlock because
	// Skyrim never calls GetKeyboardText() until it sees the event first.
	if (g_pendingKeyboardDispatch) {
		vr::VREvent_Keyboard_t data = { 0 };
		data.uUserValue = g_pendingKeyboardUserValue;

		vr::VREvent_t evt = { 0 };
		evt.eventType = vr::VREvent_KeyboardDone;
		evt.trackedDeviceIndex = 0;
		evt.data.keyboard = data;

		g_pendingKeyboardDispatch(evt);
		g_pendingKeyboardDispatch = nullptr;  // Clear after firing
	}
}

// ============================================================================
// VR Cursor System — translates controller aim to screen cursor for PrismaUI
// Both controllers can aim and click independently. Whichever hand is pointing
// closer to the center of the screen wins cursor control that frame.
//
// Activates when:
//   1. PrismaUI keyboard is shown (g_prismaKeyboardActive), OR
//   2. An external SKSE plugin calls OC_SetVRCursorActive(true)
//      (e.g., WondernuttsUI enables it when MCM or other menus open)
// ============================================================================

// External flag — SKSE plugins call OC_SetVRCursorActive() to toggle this
static bool g_vrCursorForced = false;

// Exported API: any DLL in the process can call this to enable/disable VR cursor.
// Use case: SKSE plugin detects MCM menu opened → calls OC_SetVRCursorActive(true)
//           SKSE plugin detects MCM menu closed → calls OC_SetVRCursorActive(false)
extern "C" __declspec(dllexport) void OC_SetVRCursorActive(bool active) {
	g_vrCursorForced = active;
}

static float VRCursor_Dot(float ax, float ay, float az, float bx, float by, float bz) {
	return ax * bx + ay * by + az * bz;
}

// Per-hand trigger state for edge detection
static bool g_vrTriggerDown[2] = { false, false }; // [0]=left, [1]=right

// Which hand currently controls the cursor (-1=none, 0=left, 1=right)
static int g_vrActiveHand = -1;

// Try to project one controller's aim onto the virtual screen.
// Returns true if the aim hits the screen, and fills normX/normY (0..1).
static bool VRCursor_ProjectController(
    const vr::HmdMatrix34_t& ctrlM,
    float hmdPx, float hmdPy, float hmdPz,
    float hmdFx, float hmdFy, float hmdFz,
    float hmdRx, float hmdRy, float hmdRz,
    float hmdUx, float hmdUy, float hmdUz,
    float& normX, float& normY, float& distFromCenter)
{
	// Controller position and aim direction (-Z = forward)
	float cpX = ctrlM.m[0][3], cpY = ctrlM.m[1][3], cpZ = ctrlM.m[2][3];
	float cfX = -ctrlM.m[0][2], cfY = -ctrlM.m[1][2], cfZ = -ctrlM.m[2][2];

	// Virtual screen plane: 2m in front of HMD
	float screenDist = 2.0f;
	float pcX = hmdPx + hmdFx * screenDist;
	float pcY = hmdPy + hmdFy * screenDist;
	float pcZ = hmdPz + hmdFz * screenDist;

	float pnX = -hmdFx, pnY = -hmdFy, pnZ = -hmdFz;

	// Ray-plane intersection
	float dX = pcX - cpX, dY = pcY - cpY, dZ = pcZ - cpZ;
	float denom = VRCursor_Dot(cfX, cfY, cfZ, pnX, pnY, pnZ);
	if (fabsf(denom) < 0.001f) return false;

	float t = VRCursor_Dot(dX, dY, dZ, pnX, pnY, pnZ) / denom;
	if (t < 0.0f) return false;

	float hitX = cpX + cfX * t;
	float hitY = cpY + cfY * t;
	float hitZ = cpZ + cfZ * t;

	float locX = hitX - pcX, locY = hitY - pcY, locZ = hitZ - pcZ;
	float u = VRCursor_Dot(locX, locY, locZ, hmdRx, hmdRy, hmdRz);
	float v = VRCursor_Dot(locX, locY, locZ, hmdUx, hmdUy, hmdUz);

	// Virtual screen size at 2m (~90° FOV, 16:9)
	float screenW = 4.0f;
	float screenH = 2.25f;

	normX = (u / screenW) + 0.5f;
	normY = 1.0f - ((v / screenH) + 0.5f);

	// Distance from screen center (for picking which hand wins)
	distFromCenter = sqrtf((normX - 0.5f) * (normX - 0.5f) + (normY - 0.5f) * (normY - 0.5f));

	return (normX >= 0.0f && normX <= 1.0f && normY >= 0.0f && normY <= 1.0f);
}

void UpdatePrismaKeyboardCursor()
{
	// Cursor activates when ANY of these are true:
	//   1. PrismaUI keyboard is shown
	//   2. External SKSE plugin called OC_SetVRCursorActive(true)
	//   3. Skyrim VR has a game-pausing menu open (MCM, inventory, journal, etc.)
	if (!g_prismaKeyboardActive && !g_vrCursorForced && !IsSkyrimMenuOpen()) return;

	BaseSystem* sys = GetUnsafeBaseSystem();
	if (!sys) return;

	// Get HMD (0), left controller (1), right controller (2) poses
	vr::TrackedDevicePose_t poses[3];
	sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, poses, 3);

	if (!poses[0].bPoseIsValid) return;

	const auto& hmdM = poses[0].mDeviceToAbsoluteTracking;

	// HMD basis
	float hmdPx = hmdM.m[0][3], hmdPy = hmdM.m[1][3], hmdPz = hmdM.m[2][3];
	float hmdRx = hmdM.m[0][0], hmdRy = hmdM.m[1][0], hmdRz = hmdM.m[2][0];
	float hmdUx = hmdM.m[0][1], hmdUy = hmdM.m[1][1], hmdUz = hmdM.m[2][1];
	float hmdFx = -hmdM.m[0][2], hmdFy = -hmdM.m[1][2], hmdFz = -hmdM.m[2][2];

	// Try both controllers — pick whichever is pointing closer to screen center
	const vr::TrackedDeviceIndex_t handIdx[2] = { 1, 2 }; // left=1, right=2
	float bestNormX = 0, bestNormY = 0;
	float bestDist = 999.0f;
	int bestHand = -1;

	for (int h = 0; h < 2; h++) {
		if (!poses[handIdx[h]].bPoseIsValid) continue;

		float nx, ny, dist;
		if (VRCursor_ProjectController(
		        poses[handIdx[h]].mDeviceToAbsoluteTracking,
		        hmdPx, hmdPy, hmdPz,
		        hmdFx, hmdFy, hmdFz,
		        hmdRx, hmdRy, hmdRz,
		        hmdUx, hmdUy, hmdUz,
		        nx, ny, dist)) {
			if (dist < bestDist) {
				bestDist = dist;
				bestNormX = nx;
				bestNormY = ny;
				bestHand = h;
			}
		}
	}

	if (bestHand == -1) return;
	g_vrActiveHand = bestHand;

	// Map to game window pixel coordinates
	HWND hwnd = GetForegroundWindow();
	if (!hwnd) return;

	RECT rect;
	GetClientRect(hwnd, &rect);
	POINT origin = { 0, 0 };
	ClientToScreen(hwnd, &origin);

	int winW = rect.right - rect.left;
	int winH = rect.bottom - rect.top;
	if (winW <= 0 || winH <= 0) return;

	int pixX = origin.x + (int)(bestNormX * winW);
	int pixY = origin.y + (int)(bestNormY * winH);

	SetCursorPos(pixX, pixY);

	// Check BOTH triggers — either hand can click
	for (int h = 0; h < 2; h++) {
		if (!poses[handIdx[h]].bPoseIsValid) continue;

		vr::VRControllerState_t state = {};
		if (sys->GetControllerState(handIdx[h], &state, sizeof(state))) {
			float trigger = state.rAxis[1].x;

			if (trigger > 0.8f && !g_vrTriggerDown[h]) {
				g_vrTriggerDown[h] = true;
				INPUT inp = {};
				inp.type = INPUT_MOUSE;
				inp.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
				SendInput(1, &inp, sizeof(INPUT));
			} else if (trigger < 0.3f && g_vrTriggerDown[h]) {
				g_vrTriggerDown[h] = false;
				INPUT inp = {};
				inp.type = INPUT_MOUSE;
				inp.mi.dwFlags = MOUSEEVENTF_LEFTUP;
				SendInput(1, &inp, sizeof(INPUT));
			}
		}
	}
}

using glm::mat4;
using glm::vec3;

using namespace vr;

// Class to represent an overlay
class BaseOverlay::OverlayData {
public:
	const string key;
	string name;
	HmdColor_t colour;

	float widthMeters = 1; // default 1 meter

	float autoCurveDistanceRangeMin, autoCurveDistanceRangeMax; // WTF does this do?
	EColorSpace colourSpace = ColorSpace_Auto;
	bool visible = false; // TODO check against SteamVR
	VRTextureBounds_t textureBounds = { 0, 0, 1, 1 };
	VROverlayInputMethod inputMethod = VROverlayInputMethod_None; // TODO fire events
	HmdVector2_t mouseScale = { 1.0f, 1.0f };
	bool highQuality = false;
	uint64_t flags = 0;
	float texelAspect = 1;
	std::queue<VREvent_t> eventQueue;

	// Rendering
	Texture_t texture = {};
	XrCompositionLayerQuad layerQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	std::unique_ptr<Compositor> compositor;

	// Transform
	VROverlayTransformType transformType = VROverlayTransform_Absolute;
	union {
		struct {
			HmdMatrix34_t offset;
			TrackedDeviceIndex_t device;
		} deviceRelative;
	} transformData;

	MfMatrix4f overlayTransform{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, -1.01f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	OverlayData(string key, string name)
	    : key(key), name(name)
	{
	}
};

// TODO don't pass around handles, as it will cause
// crashes when we should merely return VROverlayError_InvalidHandle
#define OVL (*((OverlayData**)pOverlayHandle))
#define USEH()                                                                        \
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;                             \
	if (!overlay || !validOverlays.count(overlay) || !overlays.count(overlay->key)) { \
		return VROverlayError_InvalidHandle;                                          \
	}

#define USEHB()                                           \
	OverlayData* overlay = (OverlayData*)ulOverlayHandle; \
	if (!overlay || !overlays.count(overlay->key)) {      \
		return false;                                     \
	}

BaseOverlay::~BaseOverlay()
{
	for (const auto& kv : overlays) {
		if (kv.second) {
			delete kv.second;
		}
	}
}

int BaseOverlay::_BuildLayers(XrCompositionLayerBaseHeader* sceneLayer, XrCompositionLayerBaseHeader const* const*& layers)
{
	// Note that at least on MSVC, this shouldn't be doing any memory allocations
	//  unless the list is expanding from new layers.
	layerHeaders.clear();
	if (sceneLayer)
		layerHeaders.push_back(sceneLayer);

	// For performance reasons, this indicates that something is using input
	// Don't write it directly to usingInput, as that could cause race conditions between
	//  when it's initially set to false, and when it's reset to true.
	bool checkUsingInput = false;

	if (keyboard) {
#ifndef OC_XR_PORT
		layerHeaders.push_back(keyboard->Update());
#endif
		checkUsingInput = true;

		if (keyboard->IsClosed())
			HideKeyboard();
	}

	if (!oovr_global_configuration.EnableLayers()) {
		goto done;
	}

	for (const auto& kv : overlays) {
		if (kv.second) {
			OverlayData& overlay = *kv.second;

			// Skip hiddden overlays, and those without a valid texture (eg, after calling ClearOverlayTexture).
			if (!overlay.visible || overlay.texture.handle == nullptr)
				continue;

			// Quick hack to get around Boneworks creating overlays and setting them to an opacity of
			// zero to hide them. Leave 1% of margin in case of weird float issues.
			// if (overlay.colour.a < 0.01)
			//	continue;

			if ((uint64_t)overlay.layerQuad.subImage.swapchain == 0) {
				continue;
			}

			const XrRect2Di& srcSize = overlay.layerQuad.subImage.imageRect;
			if (srcSize.extent.height <= 8 && srcSize.extent.width <= 8) {
				// Hack for F1 22 which creates a low res texture to fade between scenes
				// but ends up just leaving a black square that takes up half the screen.
				continue;
			}

			// Calculate the texture's aspect ratio
			const float aspect = srcSize.extent.height > 0 ? (float)srcSize.extent.width / (float)srcSize.extent.height : 1.0f;
			// ... and use that to set the size of the overlay, as it will appear to the user
			// Note we shouldn't do this when setting the texture, as the user may change the width of
			//  the overlay without changing the texture.
			overlay.layerQuad.size.width = overlay.widthMeters * overlay.overlayTransform[0][0];
			overlay.layerQuad.size.height = overlay.widthMeters * overlay.overlayTransform[1][1] / aspect;

			// Finally, add it to the list of layers to be sent to LibOVR
			overlay.layerQuad.pose = { { 0.f, 0.f, 0.f, 1.f },
				{ overlay.overlayTransform[0][3], overlay.overlayTransform[1][3], overlay.overlayTransform[2][3] } };

			layerHeaders.push_back((XrCompositionLayerBaseHeader*)&overlay.layerQuad);
		}
	}

done:
	usingInput = checkUsingInput;
	layers = layerHeaders.data();
	return static_cast<int>(layerHeaders.size());
}

bool BaseOverlay::_HandleOverlayInput(EVREye side, TrackedDeviceIndex_t index, VRControllerState_t state)
{
	if (!usingInput)
		return true;

	if (!keyboard)
		return true;

#ifndef OC_XR_PORT
	keyboard->HandleOverlayInput(side, state, (float)ovr_GetTimeInSeconds());
#endif
	return false;
}

EVROverlayError BaseOverlay::FindOverlay(const char* pchOverlayKey, VROverlayHandle_t* pOverlayHandle)
{
	if (overlays.count(pchOverlayKey)) {
		OVL = overlays[pchOverlayKey];
		return VROverlayError_None;
	}

	// TODO is this the correct return value
	return VROverlayError_InvalidParameter;
}
EVROverlayError BaseOverlay::CreateOverlay(const char* pchOverlayKey, const char* pchOverlayName, VROverlayHandle_t* pOverlayHandle)
{
	if (overlays.count(pchOverlayKey)) {
		return VROverlayError_KeyInUse;
	}

	OverlayData* data = new OverlayData(pchOverlayKey, pchOverlayName);
	OVL = data;

	overlays[pchOverlayKey] = data;
	validOverlays.insert(data);

	data->layerQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	data->layerQuad.next = NULL;
	data->layerQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	data->layerQuad.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	data->layerQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	data->layerQuad.pose = { { 0.f, 0.f, 0.f, 1.f },
		{ 0.0f, 0.0f, -0.65f } };

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::DestroyOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();

	if (highQualityOverlay == ulOverlayHandle)
		highQualityOverlay = vr::k_ulOverlayHandleInvalid;

	overlays.erase(overlay->key);
	validOverlays.erase(overlay);
	delete overlay;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetHighQualityOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();

	highQualityOverlay = ulOverlayHandle;

	return VROverlayError_None;
}
VROverlayHandle_t BaseOverlay::GetHighQualityOverlay()
{
	if (!highQualityOverlay)
		return k_ulOverlayHandleInvalid;

	return highQualityOverlay;
}
uint32_t BaseOverlay::GetOverlayKey(VROverlayHandle_t ulOverlayHandle, char* pchValue, uint32_t unBufferSize, EVROverlayError* pError)
{
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;
	if (!overlays.count(overlay->key)) {
		if (pError)
			*pError = VROverlayError_InvalidHandle;
		if (unBufferSize != 0)
			pchValue = 0;
		return 0;
	}

	const char* key = overlay->key.c_str();
	strncpy_s(pchValue, unBufferSize, key, unBufferSize);

	if (strlen(key) >= unBufferSize && unBufferSize != 0) {
		pchValue[unBufferSize - 1] = 0;
	}

	if (pError)
		*pError = VROverlayError_None;

	// Is this supposed to include the NULL or not?
	// TODO test, this could cause some very nasty bugs
	return static_cast<uint32_t>(strlen(pchValue) + 1);
}
uint32_t BaseOverlay::GetOverlayName(VROverlayHandle_t ulOverlayHandle, VR_OUT_STRING() char* pchValue, uint32_t unBufferSize, EVROverlayError* pError)
{
	if (pError)
		*pError = VROverlayError_None;

	OverlayData* overlay = (OverlayData*)ulOverlayHandle;
	if (!overlays.count(overlay->key)) {
		if (pError)
			*pError = VROverlayError_InvalidHandle;
		if (unBufferSize != 0)
			pchValue[0] = 0;
		return 0;
	}

	const char* name = overlay->name.c_str();
	strncpy_s(pchValue, unBufferSize, name, unBufferSize);

	if (strlen(name) >= unBufferSize && unBufferSize != 0) {
		pchValue[unBufferSize - 1] = 0;
	}

	// Is this supposed to include the NULL or not?
	// TODO test, this could cause some very nasty bugs
	return static_cast<uint32_t>(strlen(pchValue) + 1);
}
EVROverlayError BaseOverlay::SetOverlayName(VROverlayHandle_t ulOverlayHandle, const char* pchName)
{
	USEH();

	overlay->name = pchName;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayImageData(VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unBufferSize, uint32_t* punWidth, uint32_t* punHeight)
{
	STUBBED();
}
const char* BaseOverlay::GetOverlayErrorNameFromEnum(EVROverlayError error)
{
#define ERR_CASE(name)          \
	case VROverlayError_##name: \
		return #name;
	switch (error) {
		ERR_CASE(None);
		ERR_CASE(UnknownOverlay);
		ERR_CASE(InvalidHandle);
		ERR_CASE(PermissionDenied);
		ERR_CASE(OverlayLimitExceeded);
		ERR_CASE(WrongVisibilityType);
		ERR_CASE(KeyTooLong);
		ERR_CASE(NameTooLong);
		ERR_CASE(KeyInUse);
		ERR_CASE(WrongTransformType);
		ERR_CASE(InvalidTrackedDevice);
		ERR_CASE(InvalidParameter);
		ERR_CASE(ThumbnailCantBeDestroyed);
		ERR_CASE(ArrayTooSmall);
		ERR_CASE(RequestFailed);
		ERR_CASE(InvalidTexture);
		ERR_CASE(UnableToLoadFile);
		ERR_CASE(KeyboardAlreadyInUse);
		ERR_CASE(NoNeighbor);
		ERR_CASE(TooManyMaskPrimitives);
		ERR_CASE(BadMaskPrimitive);
	}
#undef ERR_CASE

	string msg = "Unknown overlay error code: " + to_string(error);
	OOVR_LOG(msg.c_str());

	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle, uint32_t unPID)
{
	STUBBED();
}
uint32_t BaseOverlay::GetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool bEnabled)
{
	USEH();

	if (bEnabled) {
		overlay->flags |= 1uLL << eOverlayFlag;
	} else {
		overlay->flags &= ~(1uLL << eOverlayFlag);
	}

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool* pbEnabled)
{
	USEH();

	*pbEnabled = (overlay->flags & (1uLL << eOverlayFlag)) != 0uLL;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayColor(VROverlayHandle_t ulOverlayHandle, float fRed, float fGreen, float fBlue)
{
	USEH();

	overlay->colour.r = fRed;
	overlay->colour.g = fGreen;
	overlay->colour.b = fBlue;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayColor(VROverlayHandle_t ulOverlayHandle, float* pfRed, float* pfGreen, float* pfBlue)
{
	USEH();

	*pfRed = overlay->colour.r;
	*pfGreen = overlay->colour.g;
	*pfBlue = overlay->colour.b;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float fAlpha)
{
	USEH();

	overlay->colour.a = fAlpha;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float* pfAlpha)
{
	USEH();

	*pfAlpha = overlay->colour.a;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float fTexelAspect)
{
	USEH();

	overlay->texelAspect = fTexelAspect;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float* pfTexelAspect)
{
	USEH();

	if (!pfTexelAspect)
		OOVR_ABORT("pfTexelAspect == nullptr");

	*pfTexelAspect = overlay->texelAspect;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t unSortOrder)
{
	// TODO
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t* punSortOrder)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float fWidthInMeters)
{
	USEH();

	overlay->widthMeters = fWidthInMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float* pfWidthInMeters)
{
	USEH();

	*pfWidthInMeters = overlay->widthMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float fCurvature)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float* pfCurvature)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayAutoCurveDistanceRangeInMeters(VROverlayHandle_t ulOverlayHandle, float fMinDistanceInMeters, float fMaxDistanceInMeters)
{
	USEH();

	overlay->autoCurveDistanceRangeMin = fMinDistanceInMeters;
	overlay->autoCurveDistanceRangeMax = fMaxDistanceInMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayAutoCurveDistanceRangeInMeters(VROverlayHandle_t ulOverlayHandle, float* pfMinDistanceInMeters, float* pfMaxDistanceInMeters)
{
	USEH();

	*pfMinDistanceInMeters = overlay->autoCurveDistanceRangeMin;
	*pfMaxDistanceInMeters = overlay->autoCurveDistanceRangeMax;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle, EColorSpace eTextureColorSpace)
{
	USEH();

	overlay->colourSpace = eTextureColorSpace;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle, EColorSpace* peTextureColorSpace)
{
	USEH();

	*peTextureColorSpace = overlay->colourSpace;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, const VRTextureBounds_t* pOverlayTextureBounds)
{
	USEH();

	if (pOverlayTextureBounds)
		overlay->textureBounds = *pOverlayTextureBounds;
	else
		overlay->textureBounds = { 0, 0, 1, 1 };

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, VRTextureBounds_t* pOverlayTextureBounds)
{
	USEH();

	*pOverlayTextureBounds = overlay->textureBounds;

	return VROverlayError_None;
}
uint32_t BaseOverlay::GetOverlayRenderModel(VROverlayHandle_t ulOverlayHandle, char* pchValue, uint32_t unBufferSize, HmdColor_t* pColor, EVROverlayError* pError)
{
	if (pError)
		*pError = VROverlayError_None;

	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayRenderModel(VROverlayHandle_t ulOverlayHandle, const char* pchRenderModel, const HmdColor_t* pColor)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformType(VROverlayHandle_t ulOverlayHandle, VROverlayTransformType* peTransformType)
{
	USEH();

	*peTransformType = overlay->transformType;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToOverlayTransform)
{
	USEH();

	// TODO account for the universe origin, and if it doesn't match that currently in use then add or
	//  subtract the floor position to match it. This shouldn't usually be an issue though, as I can't
	//  imagine many apps will use a different origin for their overlays.

	overlay->transformType = VROverlayTransform_Absolute;
	S2O_om44(*pmatTrackingOriginToOverlayTransform, overlay->overlayTransform);

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin* peTrackingOrigin, HmdMatrix34_t* pmatTrackingOriginToOverlayTransform)
{
	USEH();

	if (overlay->transformType != VROverlayTransform_Absolute)
		return VROverlayError_WrongTransformType;

	O2S_om34(overlay->overlayTransform, *pmatTrackingOriginToOverlayTransform);

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unTrackedDevice, const HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform)
{
	USEH();

	overlay->transformType = VROverlayTransform_TrackedDeviceRelative;
	overlay->transformData.deviceRelative.device = unTrackedDevice;
	overlay->transformData.deviceRelative.offset = *pmatTrackedDeviceToOverlayTransform;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t* punTrackedDevice, HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTransformTrackedDeviceComponent(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unDeviceIndex, const char* pchComponentName)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformTrackedDeviceComponent(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t* punDeviceIndex, char* pchComponentName, uint32_t unComponentNameSize)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformOverlayRelative(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t* ulOverlayHandleParent, HmdMatrix34_t* pmatParentOverlayToOverlayTransform)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTransformOverlayRelative(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t ulOverlayHandleParent, const HmdMatrix34_t* pmatParentOverlayToOverlayTransform)
{
	// TODO
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformCursor(VROverlayHandle_t ulCursorOverlayHandle, const HmdVector2_t* pvHotspot)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformCursor(VROverlayHandle_t ulOverlayHandle, HmdVector2_t* pvHotspot)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTransformProjection(VROverlayHandle_t ulOverlayHandle,
    ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToOverlayTransform,
    const OOVR_VROverlayProjection_t* pProjection, EVREye eEye)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ShowOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();
	overlay->visible = true;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::HideOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();
	overlay->visible = false;
	return VROverlayError_None;
}
bool BaseOverlay::IsOverlayVisible(VROverlayHandle_t ulOverlayHandle)
{
	USEHB();
	return overlay->visible;
}
EVROverlayError BaseOverlay::GetTransformForOverlayCoordinates(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, HmdVector2_t coordinatesInOverlay, HmdMatrix34_t* pmatTransform)
{
	STUBBED();
}
bool BaseOverlay::PollNextOverlayEvent(VROverlayHandle_t ulOverlayHandle, VREvent_t* pEvent, uint32_t eventSize)
{
	USEHB();

	memset(pEvent, 0, eventSize);

	if (overlay->eventQueue.empty())
		return false;

	VREvent_t e = overlay->eventQueue.front();
	overlay->eventQueue.pop();

	memcpy(pEvent, &e, std::min((uint32_t)sizeof(e), eventSize));

	return true;
}
EVROverlayError BaseOverlay::GetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod* peInputMethod)
{
	USEH();

	if (peInputMethod)
		*peInputMethod = overlay->inputMethod;

	return VROverlayError_None;
}

EVROverlayError BaseOverlay::SetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod eInputMethod)
{
	USEH();

	overlay->inputMethod = eInputMethod;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, HmdVector2_t* pvecMouseScale)
{
	USEH();

	*pvecMouseScale = overlay->mouseScale;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t* pvecMouseScale)
{
	USEH();

	if (pvecMouseScale)
		overlay->mouseScale = *pvecMouseScale;
	else
		overlay->mouseScale = HmdVector2_t{ 1.0f, 1.0f };

	return VROverlayError_None;
}
bool BaseOverlay::ComputeOverlayIntersection(VROverlayHandle_t ulOverlayHandle, const OOVR_VROverlayIntersectionParams_t* pParams, OOVR_VROverlayIntersectionResults_t* pResults)
{
	STUBBED();
}
bool BaseOverlay::HandleControllerOverlayInteractionAsMouse(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unControllerDeviceIndex)
{
	STUBBED();
}
bool BaseOverlay::IsHoverTargetOverlay(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
VROverlayHandle_t BaseOverlay::GetGamepadFocusOverlay()
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetGamepadFocusOverlay(VROverlayHandle_t ulNewFocusOverlay)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayNeighbor(EOverlayDirection eDirection, VROverlayHandle_t ulFrom, VROverlayHandle_t ulTo)
{
	STUBBED();
}
EVROverlayError BaseOverlay::MoveGamepadFocusToNeighbor(EOverlayDirection eDirection, VROverlayHandle_t ulFrom)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, const HmdVector2_t& vCenter, float fRadius)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, HmdVector2_t* pvCenter, float* pfRadius)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, const HmdVector2_t* pvCenter, float fRadius)
{
	STUBBED();
}
EVROverlayError BaseOverlay::TriggerLaserMouseHapticVibration(VROverlayHandle_t ulOverlayHandle, float fDurationSeconds, float fFrequency, float fAmplitude)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayCursor(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t ulCursorHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t* pvCursor)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ClearOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTexture(VROverlayHandle_t ulOverlayHandle, const Texture_t* pTexture)
{
	USEH();
	overlay->texture = *pTexture;

	BackendManager::Instance().OnOverlayTexture(pTexture);

	if (!oovr_global_configuration.EnableLayers() || !BackendManager::Instance().IsGraphicsConfigured())
		return VROverlayError_None;

	if (!overlay->compositor) {
		overlay->compositor.reset(GetUnsafeBaseCompositor()->CreateCompositorAPI(pTexture));
	}

	overlay->compositor->LoadSubmitContext();
	auto revertToCallerContext = MakeScopeGuard([&]() {
		overlay->compositor->ResetSubmitContext();
	});

	overlay->compositor->Invoke(&overlay->texture, nullptr);

	overlay->layerQuad.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	overlay->layerQuad.subImage = {
		overlay->compositor->GetSwapChain(),
		{ { 0, 0 },
		    { (int32_t)overlay->compositor->GetSrcSize().width,
		        (int32_t)overlay->compositor->GetSrcSize().height } },
		0
	};

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::ClearOverlayTexture(VROverlayHandle_t ulOverlayHandle)
{
	USEH();
	overlay->texture = {};

	overlay->compositor.reset();
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayRaw(VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unWidth, uint32_t unHeight, uint32_t unDepth)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayFromFile(VROverlayHandle_t ulOverlayHandle, const char* pchFilePath)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTexture(VROverlayHandle_t ulOverlayHandle, void** pNativeTextureHandle, void* pNativeTextureRef, uint32_t* pWidth, uint32_t* pHeight, uint32_t* pNativeFormat, ETextureType* pAPIType, EColorSpace* pColorSpace, VRTextureBounds_t* pTextureBounds)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ReleaseNativeOverlayHandle(VROverlayHandle_t ulOverlayHandle, void* pNativeTextureHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTextureSize(VROverlayHandle_t ulOverlayHandle, uint32_t* pWidth, uint32_t* pHeight)
{
	STUBBED();
}
EVROverlayError BaseOverlay::CreateDashboardOverlay(const char* pchOverlayKey, const char* pchOverlayFriendlyName, VROverlayHandle_t* pMainHandle, VROverlayHandle_t* pThumbnailHandle)
{
	STUBBED();
}
bool BaseOverlay::IsDashboardVisible()
{
	// TODO should this be based of whether Dash is open?
	// Probably, but handling focus opens some other issues as it triggers under other conditions.
	return false;
}
bool BaseOverlay::IsActiveDashboardOverlay(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t unProcessId)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t* punProcessId)
{
	STUBBED();
}
void BaseOverlay::ShowDashboard(const char* pchOverlayToShow)
{
	STUBBED();
}
TrackedDeviceIndex_t BaseOverlay::GetPrimaryDashboardDevice()
{
	STUBBED();
}
EVROverlayError BaseOverlay::ShowKeyboardWithDispatch(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, bool bUseMinimalMode, uint64_t uUserValue,
    VRKeyboard::eventDispatch_t eventDispatch)
{
	// Try PrismaUI keyboard for Skyrim VR
	if (InitPrismaUI() && g_prisma && g_keyboardView) {
		// Store the dispatch callback in static globals so the OnPrismaKeyboardSubmit
		// callback can fire VREvent_KeyboardDone directly into the game's event queue
		g_pendingKeyboardDispatch = eventDispatch;
		g_pendingKeyboardUserValue = uUserValue;
		g_prismaKeyboardActive = true;
		g_prismaKeyboardDone = false;
		g_prismaKeyboardResult.clear();

		// Pre-fill existing text if provided (escape single quotes for JS)
		if (pchExistingText && pchExistingText[0] != '\0') {
			std::string escaped;
			for (const char* p = pchExistingText; *p; ++p) {
				if (*p == '\'' || *p == '\\') escaped += '\\';
				escaped += *p;
			}
			std::string js = "window.setText('";
			js += escaped;
			js += "')";
			g_prisma->Invoke(g_keyboardView, js.c_str(), nullptr);
		}

		g_prisma->Show(g_keyboardView);
		g_prisma->Focus(g_keyboardView, true, false);

		return VROverlayError_None;
	}

	// Fallback: PrismaUI not available — use placeholder to prevent crash
	SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, eventDispatch, uUserValue);
	keyboardCache = "Adventurer";

#ifndef OC_XR_PORT
	if (!BaseCompositor::dxcomp)
		OOVR_ABORT("Keyboard currently only available on DX11 and DX10 games");

	if (eLineInputMode != k_EGamepadTextInputLineModeSingleLine)
		OOVR_ABORTF("Only single-line keyboard entry mode is currently supported (as opposed to ID=%d)", eLineInputMode);

	keyboard = make_unique<VRKeyboard>(BaseCompositor::dxcomp->GetDevice(), uUserValue, unCharMax, bUseMinimalMode, eventDispatch,
	    (VRKeyboard::EGamepadTextInputMode)eInputMode);

	keyboard->contents(VRKeyboard::CHAR_CONV.from_bytes(pchExistingText));

	BaseSystem* system = GetUnsafeBaseSystem();
	if (system) {
		system->_BlockInputsUntilReleased();
	}
#endif

	return VROverlayError_None;
}

/** Placeholder method for submitting a KeyboardDone event when asked to show the keyboard since it is not implemented yet. **/
void BaseOverlay::SubmitPlaceholderKeyboardEvent(vr::EVREventType ev, VRKeyboard::eventDispatch_t eventDispatch, uint64_t userValue)
{
	VREvent_Keyboard_t data = { 0 };
	data.uUserValue = userValue;

	VREvent_t evt = { 0 };
	evt.eventType = ev;
	evt.trackedDeviceIndex = 0;
	evt.data.keyboard = data;

	eventDispatch(evt);
}

EVROverlayError BaseOverlay::ShowKeyboard(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, bool bUseMinimalMode, uint64_t uUserValue)
{

	VRKeyboard::eventDispatch_t dispatch = [](VREvent_t ev) {
		BaseSystem* sys = GetUnsafeBaseSystem();
		if (sys) {
			sys->_EnqueueEvent(ev);
		}
	};

	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText, bUseMinimalMode, uUserValue, dispatch);
}
EVROverlayError BaseOverlay::ShowKeyboard(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, uint64_t uUserValue)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle,
    EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText,
    bool bUseMinimalMode, uint64_t uUserValue)
{

	USEH();

	VRKeyboard::eventDispatch_t dispatch = [overlay](VREvent_t ev) {
		overlay->eventQueue.push(ev);
	};

	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText, bUseMinimalMode, uUserValue, dispatch);
}
EVROverlayError BaseOverlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle, EGamepadTextInputMode eInputMode,
    EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char* pchDescription, uint32_t unCharMax,
    const char* pchExistingText, uint64_t uUserValue)
{
	STUBBED();
}
uint32_t BaseOverlay::GetKeyboardText(char* pchText, uint32_t cchText)
{
	// If PrismaUI keyboard submitted a result, use it.
	// VREvent_KeyboardDone was already fired by OnPrismaKeyboardSubmit() directly
	// into the event queue, so Skyrim has already received the event before calling
	// this function. We just need to hand back the text.
	if (g_prismaKeyboardDone) {
		keyboardCache = g_prismaKeyboardResult;
		g_prismaKeyboardDone = false;
	}

	string str = keyboard ? VRKeyboard::CHAR_CONV.to_bytes(keyboard->contents()) : keyboardCache;

	strncpy_s(pchText, cchText, str.c_str(), cchText);
	pchText[cchText - 1] = 0;

	return (uint32_t)strlen(pchText);
}
void BaseOverlay::HideKeyboard()
{
	// If PrismaUI keyboard is active, hide it and clear state
	if (g_prismaKeyboardActive && g_prisma && g_keyboardView) {
		g_prisma->Unfocus(g_keyboardView);
		g_prisma->Hide(g_keyboardView);
		g_prismaKeyboardActive = false;
		g_pendingKeyboardDispatch = nullptr;
		return;
	}

	// First, if the keyboard is currently open, cache it's contents
	if (keyboard) {
		keyboardCache = VRKeyboard::CHAR_CONV.to_bytes(keyboard->contents());
	}

	// Delete the keyboard instance
	keyboard.reset();

	BaseSystem* system = GetUnsafeBaseSystem();
	if (system) {
		system->_BlockInputsUntilReleased();
	}
}
void BaseOverlay::SetKeyboardTransformAbsolute(ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToKeyboardTransform)
{
	if (!keyboard)
		OOVR_ABORT("Cannot set keyboard position when the keyboard is closed!");

#ifdef OC_XR_PORT
	XR_STUBBED();
#else
	BaseCompositor* compositor = GetUnsafeBaseCompositor();
	if (compositor && eTrackingOrigin != compositor->GetTrackingSpace()) {
		OOVR_ABORTF("Origin mismatch - current %d, requested %d", compositor->GetTrackingSpace(), eTrackingOrigin);
	}

	keyboard->SetTransform(*pmatTrackingOriginToKeyboardTransform);
#endif
}
void BaseOverlay::SetKeyboardPositionForOverlay(VROverlayHandle_t ulOverlayHandle, HmdRect2_t avoidRect)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayIntersectionMask(VROverlayHandle_t ulOverlayHandle, OOVR_VROverlayIntersectionMaskPrimitive_t* pMaskPrimitives, uint32_t unNumMaskPrimitives, uint32_t unPrimitiveSize)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayFlags(VROverlayHandle_t ulOverlayHandle, uint32_t* pFlags)
{
	STUBBED();
}
BaseOverlay::VRMessageOverlayResponse BaseOverlay::ShowMessageOverlay(const char* pchText, const char* pchCaption, const char* pchButton0Text, const char* pchButton1Text, const char* pchButton2Text, const char* pchButton3Text)
{
	STUBBED();
}
void BaseOverlay::CloseMessageOverlay()
{
	STUBBED();
}

EVROverlayError BaseOverlay::SetOverlayPreCurvePitch(vr::VROverlayHandle_t ulOverlayHandle, float fRadians)
{
	STUBBED();
}

EVROverlayError BaseOverlay::GetOverlayPreCurvePitch(vr::VROverlayHandle_t ulOverlayHandle, float* pfRadians)
{
	STUBBED();
}

EVROverlayError BaseOverlay::WaitFrameSync(uint32_t nTimeoutMs)
{
	STUBBED();
}
