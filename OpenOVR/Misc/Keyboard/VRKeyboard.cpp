#include "stdafx.h"

#include "VRKeyboard.h"

#include <d3d11.h>

#include "Reimpl/BaseCompositor.h"
#include "Reimpl/BaseInput.h"
#include "Reimpl/BaseSystem.h"
#include "generated/static_bases.gen.h"

#include "Misc/ScopeGuard.h"
#include "convert.h"

#include "resources.h"

#include <vector>

#ifdef _WIN32
#pragma comment(lib, "d3d11.lib")

// for debugging only for now
#include <comdef.h>
#endif

static std::vector<char> loadResource(int rid, int type)
{
#ifdef _WIN32
	// Open our OBJ file
	HRSRC ref = FindResource(openovr_module_id, MAKEINTRESOURCE(rid), MAKEINTRESOURCE(type));
	if (!ref) {
		string err = "FindResource error: " + std::to_string(GetLastError());
		OOVR_ABORT(err.c_str());
	}

	char* cstr = (char*)LoadResource(openovr_module_id, ref);
	if (!cstr) {
		string err = "LoadResource error: " + std::to_string(GetLastError());
		OOVR_ABORT(err.c_str());
	}

	DWORD len = SizeofResource(openovr_module_id, ref);
	if (!len) {
		string err = "SizeofResource error: " + std::to_string(GetLastError());
		OOVR_ABORT(err.c_str());
	}

	return std::vector<char>(cstr, cstr + len);
#else
	OOVR_ABORT("Keyboard font loading not implemented on this platform");
	return {};
#endif
}

std::wstring_convert<std::codecvt_utf8<wchar_t>> VRKeyboard::CHAR_CONV;

VRKeyboard::VRKeyboard(ID3D11Device* dev, uint64_t userValue, uint32_t maxLength, bool minimal, eventDispatch_t eventDispatch,
    EGamepadTextInputMode inputMode)
    : dev(dev), userValue(userValue), maxLength(maxLength), minimal(minimal), eventDispatch(eventDispatch), inputMode(inputMode)
{

	std::shared_ptr<BaseCompositor> cmp = GetBaseCompositor();
	if (!cmp)
		OOVR_ABORT("Keyboard: Compositor must be active!");

	if (!dev)
		OOVR_ABORT("Keyboard currently only works on DX11, and game must have submitted at least a single frame");

	if (inputMode == EGamepadTextInputMode::k_EGamepadTextInputModePassword)
		OOVR_ABORT("Password input mode not yet supported!");

	// zero stuff out
	memset(lastInputTime, 0, sizeof(lastInputTime));
	memset(repeatCount, 0, sizeof(repeatCount));
	memset(selected, 0, sizeof(selected));
	memset(lastButtonState, 0, sizeof(lastButtonState));

	// D3D setup
	dev->GetImmediateContext(&ctx);

	// Create OpenXR swap chain for the keyboard texture
	XrSwapchainCreateInfo swapchainInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	swapchainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = texWidth;
	swapchainInfo.height = texHeight;
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;

	OOVR_FAILED_XR_ABORT(xrCreateSwapchain(xr_session.get(), &swapchainInfo, &chain));

	// Enumerate swap chain images
	uint32_t imageCount = 0;
	OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr));

	swapchainImages.resize(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain,
	    swapchainImages.size(), &imageCount, (XrSwapchainImageBaseHeader*)swapchainImages.data()));

	// Set up the OpenXR composition layer quad — HEAD-LOCKED
	// Using viewSpace means the keyboard stays fixed in front of the player's face
	// regardless of head movement.
	memset(&layer, 0, sizeof(layer));
	layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	layer.next = nullptr;
	layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	layer.space = xr_gbl->viewSpace; // Head-locked: moves with the headset
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.subImage.swapchain = chain;
	layer.subImage.imageRect.offset = { 0, 0 };
	layer.subImage.imageRect.extent = { (int32_t)texWidth, (int32_t)texHeight };
	layer.subImage.imageArrayIndex = 0;

	// 80cm wide, 48cm tall — positioned 80cm in front, below eye level
	// Pushed further back so the keyboard sits behind outstretched hands
	layer.size.width = 0.80f;
	layer.size.height = 0.48f;
	layer.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f }; // Identity — facing the viewer
	layer.pose.position = { 0.0f, -0.35f, -0.80f }; // Centered, lowered — behind outstretched hands

	font = make_unique<SudoFontMeta>(loadResource(RES_O_FNT_UBUNTU, RES_T_FNTMETA), loadResource(RES_O_FNT_UBUNTU, RES_T_PNG));
	layout = make_unique<KeyboardLayout>(loadResource(RES_O_KB_EN_GB, RES_T_KBLAYOUT));

	// Create laser beam swapchains — tiny solid-color textures, one per hand
	for (int i = 0; i < 2; i++) {
		XrSwapchainCreateInfo laserSci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		laserSci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		laserSci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		laserSci.sampleCount = 1;
		laserSci.width = 4;
		laserSci.height = 4;
		laserSci.faceCount = 1;
		laserSci.arraySize = 1;
		laserSci.mipCount = 1;

		OOVR_FAILED_XR_ABORT(xrCreateSwapchain(xr_session.get(), &laserSci, &laserChain[i]));

		uint32_t laserImgCount = 0;
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(laserChain[i], 0, &laserImgCount, nullptr));
		std::vector<XrSwapchainImageD3D11KHR> laserImgs(laserImgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(laserChain[i], laserImgCount, &laserImgCount,
		    (XrSwapchainImageBaseHeader*)laserImgs.data()));

		// Left = enchantment blue, Right = alchemy green — semi-transparent
		uint8_t cr = (i == 0) ? 80 : 60, cg = (i == 0) ? 140 : 220, cb = (i == 0) ? 255 : 80, ca = 180;
		uint32_t packed = cr | (cg << 8) | (cb << 16) | (ca << 24);
		uint32_t colorPixels[16];
		for (int j = 0; j < 16; j++) colorPixels[j] = packed;

		D3D11_TEXTURE2D_DESC ltd = {};
		ltd.Width = 4;
		ltd.Height = 4;
		ltd.MipLevels = 1;
		ltd.ArraySize = 1;
		ltd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		ltd.SampleDesc = { 1, 0 };
		ltd.Usage = D3D11_USAGE_DEFAULT;

		D3D11_SUBRESOURCE_DATA linit = { colorPixels, sizeof(uint32_t) * 4, sizeof(uint32_t) * 16 };
		CComPtr<ID3D11Texture2D> ltex;
		OOVR_FAILED_DX_ABORT(dev->CreateTexture2D(&ltd, &linit, &ltex));

		XrSwapchainImageAcquireInfo lacq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t lidx = 0;
		OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(laserChain[i], &lacq, &lidx));
		XrSwapchainImageWaitInfo lwait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		lwait.timeout = 500000000;
		OOVR_FAILED_XR_ABORT(xrWaitSwapchainImage(laserChain[i], &lwait));
		ctx->CopyResource(laserImgs[lidx].texture, ltex);
		XrSwapchainImageReleaseInfo lrel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(laserChain[i], &lrel));

		// Initialize the laser composition layer
		memset(&laserLayer[i], 0, sizeof(laserLayer[i]));
		laserLayer[i].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		laserLayer[i].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		laserLayer[i].space = xr_gbl->viewSpace;
		laserLayer[i].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		laserLayer[i].subImage.swapchain = laserChain[i];
		laserLayer[i].subImage.imageRect.offset = { 0, 0 };
		laserLayer[i].subImage.imageRect.extent = { 4, 4 };
		laserLayer[i].subImage.imageArrayIndex = 0;
	}
}

VRKeyboard::~VRKeyboard()
{
	for (int i = 0; i < 2; i++) {
		if (laserChain[i] != XR_NULL_HANDLE) {
			xrDestroySwapchain(laserChain[i]);
			laserChain[i] = XR_NULL_HANDLE;
		}
	}
	if (chain != XR_NULL_HANDLE) {
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
	}
	if (ctx)
		ctx->Release();
}

wstring VRKeyboard::contents()
{
	return text;
}

void VRKeyboard::contents(wstring str)
{
	text = str;
	dirty = true;
}

// Convert thumbstick axis values into D-pad button bits so the keyboard
// navigation code (which checks k_EButton_DPad_*) works with Quest Touch
// controllers that only report thumbstick as analog axes.
static void InjectThumbstickAsDpad(vr::VRControllerState_t& state, float deadzone = 0.5f)
{
	// Axis 0 is the joystick/thumbstick in OpenComposite's mapping.
	// The keyboard layout uses Left/Right for horizontal navigation and
	// Up/Down for row navigation. Quest thumbstick axes are remapped:
	//   Stick Left/Right (X axis) → D-pad Up/Down (row navigation)
	//   Stick Up/Down (Y axis)    → D-pad Left/Right (key navigation)
	float x = state.rAxis[0].x;
	float y = state.rAxis[0].y;

	if (y > deadzone)
		state.ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Right);
	if (y < -deadzone)
		state.ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Left);
	if (x < -deadzone)
		state.ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Down);
	if (x > deadzone)
		state.ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Up);
}

// Compute a quaternion that orients a quad so its Y axis aligns with beamDir
// and the quad faces the viewer (who is at origin in viewSpace).
static XrQuaternionf beamOrientation(XrVector3f beamDir, XrVector3f midpoint)
{
	// up = beamDir (height axis of quad)
	XrVector3f up = beamDir;

	// forward_raw = from midpoint toward viewer (origin in viewSpace)
	float ml = sqrtf(midpoint.x * midpoint.x + midpoint.y * midpoint.y + midpoint.z * midpoint.z);
	XrVector3f fwd;
	if (ml > 0.001f) {
		fwd = { -midpoint.x / ml, -midpoint.y / ml, -midpoint.z / ml };
	} else {
		fwd = { 0, 0, 1 };
	}

	// right = cross(up, fwd)
	XrVector3f right = {
		up.y * fwd.z - up.z * fwd.y,
		up.z * fwd.x - up.x * fwd.z,
		up.x * fwd.y - up.y * fwd.x
	};
	float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
	if (rl < 0.001f) {
		right = { 1, 0, 0 };
		rl = 1.0f;
	}
	right.x /= rl; right.y /= rl; right.z /= rl;

	// Re-orthogonalize forward = cross(right, up)
	fwd = {
		right.y * up.z - right.z * up.y,
		right.z * up.x - right.x * up.z,
		right.x * up.y - right.y * up.x
	};

	// Rotation matrix [right | up | fwd] as columns → quaternion
	// R = | right.x  up.x  fwd.x |
	//     | right.y  up.y  fwd.y |
	//     | right.z  up.z  fwd.z |
	float trace = right.x + up.y + fwd.z;
	XrQuaternionf q;

	if (trace > 0) {
		float s = 0.5f / sqrtf(trace + 1.0f);
		q.w = 0.25f / s;
		q.x = (up.z - fwd.y) * s;
		q.y = (fwd.x - right.z) * s;
		q.z = (right.y - up.x) * s;
	} else if (right.x > up.y && right.x > fwd.z) {
		float s = 2.0f * sqrtf(1.0f + right.x - up.y - fwd.z);
		q.w = (up.z - fwd.y) / s;
		q.x = 0.25f * s;
		q.y = (up.x + right.y) / s;
		q.z = (fwd.x + right.z) / s;
	} else if (up.y > fwd.z) {
		float s = 2.0f * sqrtf(1.0f + up.y - right.x - fwd.z);
		q.w = (fwd.x - right.z) / s;
		q.x = (up.x + right.y) / s;
		q.y = 0.25f * s;
		q.z = (fwd.y + up.z) / s;
	} else {
		float s = 2.0f * sqrtf(1.0f + fwd.z - right.x - up.y);
		q.w = (right.y - up.x) / s;
		q.x = (fwd.x + right.z) / s;
		q.y = (fwd.y + up.z) / s;
		q.z = 0.25f * s;
	}

	return q;
}

void VRKeyboard::UpdateLaserBeam(int side)
{
	if (!laserActive[side])
		return;

	XrVector3f A = laserOrigin[side];
	XrVector3f B = laserHitPoint[side];

	float dx = B.x - A.x, dy = B.y - A.y, dz = B.z - A.z;
	float fullLen = sqrtf(dx * dx + dy * dy + dz * dz);
	if (fullLen < 0.01f) {
		laserActive[side] = false;
		return;
	}

	// Beam extends 60% of the way — stops short of the keyboard (Virtual Desktop style)
	float beamFraction = 0.6f;
	float beamLen = fullLen * beamFraction;

	XrVector3f beamEnd = {
		A.x + dx * beamFraction,
		A.y + dy * beamFraction,
		A.z + dz * beamFraction
	};

	XrVector3f dir = { dx / fullLen, dy / fullLen, dz / fullLen };
	XrVector3f mid = {
		(A.x + beamEnd.x) * 0.5f,
		(A.y + beamEnd.y) * 0.5f,
		(A.z + beamEnd.z) * 0.5f
	};

	laserLayer[side].pose.position = mid;
	laserLayer[side].size.width = 0.003f; // 3mm thin
	laserLayer[side].size.height = beamLen;
	laserLayer[side].pose.orientation = beamOrientation(dir, mid);
}

const std::vector<XrCompositionLayerBaseHeader*>& VRKeyboard::Update()
{
	activeLayers.clear();

	// Poll controller input directly — _HandleOverlayInput is never called
	// by the engine, so the keyboard must poll its own input each frame.
	BaseSystem* sys = GetUnsafeBaseSystem();
	if (sys) {
		float time = (float)(GetTickCount64() / 1000.0);

		// Laser pointer selection — point controllers at the keyboard to select keys.
		// Done before HandleOverlayInput so trigger activates the laser-pointed key.
		bool anyLaserActive = false;
		for (int side = 0; side < 2; side++) {
			int hitKey = HitTestLaser(side);
			if (hitKey >= 0 && hitKey != selected[side]) {
				selected[side] = hitKey;
				dirty = true;
			}
			if (laserActive[side]) {
				anyLaserActive = true;
				UpdateLaserBeam(side);
			}
		}

		// Re-render keyboard every frame when laser is active (for cursor dot)
		if (anyLaserActive)
			dirty = true;

		// Left controller (device index 1)
		vr::VRControllerState_t stateL = {};
		if (sys->GetControllerState(1, &stateL, sizeof(stateL))) {
			InjectThumbstickAsDpad(stateL);
			HandleOverlayInput(vr::Eye_Left, stateL, time);
		}

		// Right controller (device index 2)
		vr::VRControllerState_t stateR = {};
		if (sys->GetControllerState(2, &stateR, sizeof(stateR))) {
			InjectThumbstickAsDpad(stateR);
			HandleOverlayInput(vr::Eye_Right, stateR, time);
		}
	}

	if (dirty) {
		dirty = false;
		Refresh();
	}

	// Build layer list: keyboard first, then any active laser beams
	activeLayers.push_back((XrCompositionLayerBaseHeader*)&layer);
	for (int side = 0; side < 2; side++) {
		if (laserActive[side])
			activeLayers.push_back((XrCompositionLayerBaseHeader*)&laserLayer[side]);
	}

	return activeLayers;
}

int VRKeyboard::HitTestLaser(int side)
{
	laserActive[side] = false;

	std::shared_ptr<BaseInput> input = GetBaseInput();
	if (!input || !input->AreActionsLoaded())
		return -1;

	// Get the aim space for this controller (device index = side + 1)
	XrSpace aimSpace = XR_NULL_HANDLE;
	input->GetHandSpace((vr::TrackedDeviceIndex_t)(side + 1), aimSpace, true);

	if (aimSpace == XR_NULL_HANDLE)
		return -1;

	// Locate the aim space relative to viewSpace (the keyboard's coordinate system)
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	XrResult result = xrLocateSpace(aimSpace, xr_gbl->viewSpace,
	    xr_gbl->GetBestTime(), &location);

	if (XR_FAILED(result)
	    || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
	    || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return -1;

	// Ray origin = controller aim position in viewSpace
	XrVector3f rayOrigin = location.pose.position;

	// Ray direction = forward vector (-Z) of the aim pose, rotated by orientation
	XrVector3f forward = { 0.0f, 0.0f, -1.0f };
	XrVector3f rayDir;
	rotate_vector_by_quaternion(forward, location.pose.orientation, rayDir);

	// Keyboard plane: normal is +Z (quad faces the viewer), at z = layer.pose.position.z
	float planeZ = layer.pose.position.z;
	float denom = rayDir.z;
	if (fabsf(denom) < 1e-6f)
		return -1; // ray parallel to plane

	float t = (planeZ - rayOrigin.z) / denom;
	if (t <= 0.0f)
		return -1; // intersection behind the ray

	// Hit point on the keyboard plane
	float hitX = rayOrigin.x + t * rayDir.x;
	float hitY = rayOrigin.y + t * rayDir.y;

	// Convert to normalized keyboard coordinates [0,1]
	float localX = hitX - layer.pose.position.x;
	float localY = hitY - layer.pose.position.y;
	float u = (localX + layer.size.width * 0.5f) / layer.size.width;
	float v = (localY + layer.size.height * 0.5f) / layer.size.height;

	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return -1; // missed the quad

	// Store laser hit data for beam rendering and cursor dot
	laserActive[side] = true;
	laserOrigin[side] = rayOrigin;
	laserHitPoint[side] = { hitX, hitY, planeZ };
	laserU[side] = u;
	laserV[side] = v;

	// Convert to pixel coordinates (texture Y is flipped: top of quad = top of texture)
	int texX = (int)(u * texWidth);
	int texY = (int)((1.0f - v) * texHeight);

	// Hit-test against keyboard keys
	int padding = 8;
	int kbWidth = layout->GetWidth();
	int keySize = (((int)texWidth - padding) / kbWidth) - padding;
	int keyAreaBaseY = minimal ? padding : padding + keySize + padding;

	for (const auto& key : layout->GetKeymap()) {
		int kx = padding + (int)((keySize + padding) * key.x);
		int ky = keyAreaBaseY + (int)((keySize + padding) * key.y);
		int kw = (int)(keySize * key.w);
		int kh = (int)(keySize * key.h);
		if (key.spansToRight)
			kw = (int)texWidth - padding - kx;

		if (texX >= kx && texX < kx + kw && texY >= ky && texY < ky + kh)
			return key.id;
	}

	return -1; // hit the quad but not a key
}

void VRKeyboard::HandleOverlayInput(vr::EVREye side, vr::VRControllerState_t state, float time)
{
	using namespace vr;

	// In case this is somehow called after the keyboard is closed, ignore it
	if (IsClosed())
		return;

	uint64_t lastButtons = lastButtonState[side];
	lastButtonState[side] = state.ulButtonPressed;

#define GET_BTTN(var, key) bool var = state.ulButtonPressed & ButtonMaskFromId(key)
#define GET_BTTN_LAST(var, key) \
	GET_BTTN(var, key);         \
	bool var##_last = lastButtons & ButtonMaskFromId(key)
	GET_BTTN(left, k_EButton_DPad_Left);
	GET_BTTN(right, k_EButton_DPad_Right);
	GET_BTTN(up, k_EButton_DPad_Up);
	GET_BTTN(down, k_EButton_DPad_Down);
	GET_BTTN_LAST(trigger, k_EButton_SteamVR_Trigger);
	GET_BTTN_LAST(grip, k_EButton_Grip);
#undef GET_BTTN
#undef GET_BTTN_LAST

	if (grip && !grip_last) {
		closed = true;
		SubmitEvent(VREvent_KeyboardClosed, 0);
		return;
	}

	const KeyboardLayout::Key& key = layout->GetKeymap()[selected[side]];

	if (trigger && !trigger_last) {
		wchar_t ch = caseMode == ECaseMode::LOWER ? key.ch : key.shift;

		bool submitKeyEvent = false;

		if (ch == '\x01' || ch == '\x02') {
			// Shift
			ECaseMode target = ch == '\x02' ? ECaseMode::LOCK : ECaseMode::SHIFT;
			caseMode = caseMode == target ? ECaseMode::LOWER : target;
		} else if (ch == '\b') {
			// Backspace
			if (!text.empty()) {
				text.erase(text.end() - 1);
			}

			submitKeyEvent = true;
		} else if (ch == '\x03') {
			// done

			// Submit mode is for stuff like chat, where the keyboard stays open
			if (inputMode != EGamepadTextInputMode::k_EGamepadTextInputModeSubmit)
				closed = true;

			if (!minimal)
				SubmitEvent(VREvent_KeyboardCharInput, 0);

			SubmitEvent(VREvent_KeyboardDone, 0);
		} else if (!minimal && ch == '\t') {
			// Silently soak up tabs for now
		} else if (!minimal && ch == '\n') {
			// Silently soak up newlines for now
		} else {
			text += ch;

			submitKeyEvent = true;

			if (caseMode == ECaseMode::SHIFT)
				caseMode = ECaseMode::LOWER;
		}

		if (submitKeyEvent) {
			SubmitEvent(VREvent_KeyboardCharInput, minimal ? ch : 0);
		}

		dirty = true;
	}

	// Movement:
	bool any = left || right || up || down;
	if (!any) {
	cancel:
		repeatCount[side] = 0;
		lastInputTime[side] = 0;
		return;
	}

	if (time - lastInputTime[side] < (repeatCount[side] <= 1 ? 0.3 : 0.1))
		return;

	lastInputTime[side] = time;
	repeatCount[side]++;

	int target = -1;
	if (left)
		target = key.toLeft;
	else if (right)
		target = key.toRight;
	else if (up)
		target = key.toUp;
	else if (down)
		target = key.toDown;

	if (target == -1) {
		goto cancel;
	}

	selected[side] = target;
	dirty = true;
}

void VRKeyboard::SetTransform(vr::HmdMatrix34_t transform)
{
	layer.pose = S2O_om34_pose(transform);
}

struct pix_t {
	uint8_t r, g, b, a;
};

static_assert(sizeof(pix_t) == 4, "padded pix_t");

void VRKeyboard::Refresh()
{
	D3D11_TEXTURE2D_DESC desc;
	desc.Width = texWidth;
	desc.Height = texHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	pix_t* pixels = new pix_t[desc.Width * desc.Height];

	// ── Skyrim-inspired color palette ──
	//  Dark stone/leather background, golden accents, parchment text
	const int BORD = 2; // Key border thickness in pixels

	// Fill background — dark charcoal with warm brown undertone
	for (UINT y = 0; y < desc.Height; y++) {
		for (UINT x = 0; x < desc.Width; x++) {
			pix_t& pix = pixels[x + y * desc.Width];
			pix.r = 30; pix.g = 26; pix.b = 22; pix.a = 255;
		}
	}

	int padding = 8;

	auto fillArea = [pixels, &desc](int x, int y, int w, int h, int r, int g, int b) {
		for (int ix = 0; ix < w; ix++) {
			for (int iy = 0; iy < h; iy++) {
				int px = x + ix;
				int py = y + iy;
				if (px < 0 || py < 0 || px >= (int)desc.Width || py >= (int)desc.Height)
					continue;
				pix_t& p = pixels[px + py * desc.Width];
				p.r = r; p.g = g; p.b = b; p.a = 255;
			}
		}
	};

	auto print = [&](int x, int y, pix_t colour, wstring text, bool hpad = true) {
		SudoFontMeta::pix_t c = { colour.r, colour.g, colour.b, colour.a };
		for (size_t i = 0; i < text.length(); i++) {
			font->Blit(text[i], x, y, desc.Width, c, (SudoFontMeta::pix_t*)pixels, hpad);
			x += font->Width(text[i]);
		}
	};

	// Draw a thin golden frame around the entire keyboard
	fillArea(0, 0, desc.Width, BORD, 140, 115, 45);                    // top
	fillArea(0, desc.Height - BORD, desc.Width, BORD, 140, 115, 45);   // bottom
	fillArea(0, 0, BORD, desc.Height, 140, 115, 45);                   // left
	fillArea(desc.Width - BORD, 0, BORD, desc.Height, 140, 115, 45);   // right

	int kbWidth = layout->GetWidth();
	int keySize = ((desc.Width - padding) / kbWidth) - padding;
	auto drawKey = [&](int x, int y, const KeyboardLayout::Key& key) {
		int width = (int)(keySize * key.w);
		int height = (int)(keySize * key.h);

		if (key.spansToRight) {
			width = desc.Width - padding - x;
		}

		bool highlighted = (key.ch == '\x01' && caseMode == ECaseMode::SHIFT)
		    || (key.ch == '\x02' && caseMode == ECaseMode::LOCK);

		// Golden border around every key
		fillArea(x, y, width, height, 140, 115, 45);

		// Key interior (inset by border)
		if (highlighted) {
			// Active shift/caps — warm golden interior
			fillArea(x + BORD, y + BORD, width - BORD * 2, height - BORD * 2, 160, 130, 50);
		} else {
			// Normal key — dark stone
			fillArea(x + BORD, y + BORD, width - BORD * 2, height - BORD * 2, 50, 45, 38);
		}

		// Controller selection highlights (inside the border)
		bool leftSel = (selected[vr::Eye_Left] == key.id);
		bool rightSel = (selected[vr::Eye_Right] == key.id);

		if (leftSel && rightSel) {
			// Both on same key — split
			int halfW = (width - BORD * 2) / 2;
			fillArea(x + BORD, y + BORD, halfW, height - BORD * 2, 30, 60, 120);
			fillArea(x + BORD + halfW, y + BORD, width - BORD * 2 - halfW, height - BORD * 2, 30, 100, 55);
		} else if (leftSel) {
			// Left controller — deep enchantment blue
			fillArea(x + BORD, y + BORD, width - BORD * 2, height - BORD * 2, 30, 60, 120);
		} else if (rightSel) {
			// Right controller — deep alchemy green
			fillArea(x + BORD, y + BORD, width - BORD * 2, height - BORD * 2, 30, 100, 55);
		}

		// Text color
		pix_t targetColour;
		if (highlighted) {
			targetColour = { 30, 26, 22, 255 }; // Dark text on golden key
		} else if (leftSel || rightSel) {
			targetColour = { 240, 225, 180, 255 }; // Bright parchment on selection
		} else {
			targetColour = { 210, 190, 140, 255 }; // Warm parchment
		}

		wstring label = caseMode == ECaseMode::LOWER ? key.label : key.labelShift;
		int textWidth = font->Width(label);

		print(x + (width - textWidth) / 2, y + padding, targetColour, label, false);
	};

	int keyAreaBaseY = minimal ? padding : padding + keySize + padding;
	for (const KeyboardLayout::Key& key : layout->GetKeymap()) {
		int x = padding + (int)((keySize + padding) * key.x);
		int y = keyAreaBaseY + (int)((keySize + padding) * key.y);

		drawKey(x, y, key);
	}

	if (!minimal) {
		// Text input bar — golden border with dark interior
		fillArea(padding, padding, desc.Width - padding * 2, keySize, 140, 115, 45);
		fillArea(padding + BORD, padding + BORD, desc.Width - padding * 2 - BORD * 2, keySize - BORD * 2, 38, 33, 28);

		pix_t targetColour = { 220, 200, 155, 255 }; // Parchment text

		print(padding + BORD + 6, padding + BORD + 4, targetColour, text);
	}

	// Draw laser cursor dots on the keyboard surface
	for (int side = 0; side < 2; side++) {
		if (!laserActive[side])
			continue;

		int cx = (int)(laserU[side] * desc.Width);
		int cy = (int)((1.0f - laserV[side]) * desc.Height);
		int radius = 6;

		// Blue for left, green for right — bright so they stand out
		int cr = (side == 0) ? 100 : 80;
		int cg = (side == 0) ? 180 : 255;
		int cb = (side == 0) ? 255 : 100;

		// Filled circle
		for (int dy = -radius; dy <= radius; dy++) {
			for (int dx = -radius; dx <= radius; dx++) {
				if (dx * dx + dy * dy <= radius * radius) {
					int px = cx + dx, py = cy + dy;
					if (px >= 0 && px < (int)desc.Width && py >= 0 && py < (int)desc.Height) {
						pix_t& p = pixels[px + py * desc.Width];
						p.r = cr; p.g = cg; p.b = cb; p.a = 255;
					}
				}
			}
		}
	}

	// Create a staging texture with the CPU-rendered pixels
	D3D11_SUBRESOURCE_DATA init[] = {
		{ pixels, sizeof(pix_t) * desc.Width, sizeof(pix_t) * desc.Width * desc.Height }
	};

	CComPtr<ID3D11Texture2D> tex;
	HRESULT rres = dev->CreateTexture2D(&desc, init, &tex);
	OOVR_FAILED_DX_ABORT(rres);
	delete[] pixels;

	// Acquire an image from the OpenXR swap chain
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t currentIndex = 0;
	OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(chain, &acquireInfo, &currentIndex));

	// Wait for the image to be ready
	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = 500000000; // 500ms
	OOVR_FAILED_XR_ABORT(xrWaitSwapchainImage(chain, &waitInfo));

	// Copy the staging texture to the swap chain image
	ctx->CopyResource(swapchainImages[currentIndex].texture, tex);

	// Release the swap chain image
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(chain, &releaseInfo));
}

void VRKeyboard::SubmitEvent(vr::EVREventType ev, wchar_t ch)
{
	// Here's how (from some basic experimentation) the SteamVR keyboard appears to submit events:
	// In minimal mode:
	// * Pressing a key submits a KeyboardCharInput event, with the character stored in cNewInput
	//    TODO find out how this is encoeded with unicode characters
	// * Clicking of the keyboard submits a KeyboardClosed event, with cNewInput empty (all zeros)
	// * Clicking 'done' submits a KeyboardDone event, with cNewInput empty
	// In standard mode:
	// * cNewInput is always empty
	// * Pressing a key submits a KeyboardCharInput event (the app must read
	//    the text via GetKeyboardText if it wants to know the keyboard contents, since cNewInput is empty)
	// * Clicking of the keyboard submits a KeyboardClosed event
	// * Clicking 'done' submits a KeyboardCharInput event, followed by a KeyboardDone event

	vr::VREvent_Keyboard_t data = { 0 };
	data.uUserValue = userValue;

	memset(data.cNewInput, 0, sizeof(data.cNewInput));

	if (ch != 0) {
		string utf8 = CHAR_CONV.to_bytes(ch);

		if (utf8.length() > sizeof(data.cNewInput)) {
			OOVR_ABORTF("Cannot write symbol '%s' with too many bytes (%d bytes UTF8)", utf8.c_str(), (int)utf8.length());
		}

		memcpy(data.cNewInput, utf8.c_str(), utf8.length());
	}

	vr::VREvent_t evt = { 0 };
	evt.eventType = ev;
	evt.trackedDeviceIndex = 0; // This is accurate to SteamVR
	evt.data.keyboard = data;

	eventDispatch(evt);
}
