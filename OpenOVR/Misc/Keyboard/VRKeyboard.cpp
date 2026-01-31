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

#include <cmath>
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

	// Set up the OpenXR composition layer quad — WORLD-ANCHORED
	// Using floorSpace (stage) so the keyboard stays fixed in world space.
	// The user can grab the top bar and reposition it.
	memset(&layer, 0, sizeof(layer));
	layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	layer.next = nullptr;
	layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	layer.space = xr_gbl->floorSpace; // World-anchored: stays in place
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.subImage.swapchain = chain;
	layer.subImage.imageRect.offset = { 0, 0 };
	layer.subImage.imageRect.extent = { (int32_t)texWidth, (int32_t)texHeight };
	layer.subImage.imageArrayIndex = 0;

	layer.size.width = 0.80f;
	layer.size.height = 0.37f;

	// Spawn the keyboard in front of the player's current head position
	XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
	XrResult headResult = xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace,
	    xr_gbl->GetBestTime(), &headLoc);

	if (XR_SUCCEEDED(headResult)
	    && (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
	    && (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		// Head forward direction projected to horizontal plane (yaw only)
		XrVector3f headFwd;
		XrVector3f localFwd = { 0.0f, 0.0f, -1.0f };
		rotate_vector_by_quaternion(localFwd, headLoc.pose.orientation, headFwd);
		headFwd.y = 0;
		float fwdLen = sqrtf(headFwd.x * headFwd.x + headFwd.z * headFwd.z);
		if (fwdLen > 0.001f) {
			headFwd.x /= fwdLen;
			headFwd.z /= fwdLen;
		} else {
			headFwd = { 0.0f, 0.0f, -1.0f };
		}

		// Position: 80cm forward, 35cm below head
		layer.pose.position = {
			headLoc.pose.position.x + headFwd.x * 0.80f,
			headLoc.pose.position.y - 0.35f,
			headLoc.pose.position.z + headFwd.z * 0.80f
		};

		// Orientation: face toward the user (rotate around Y axis)
		float yaw = atan2f(headFwd.x, headFwd.z);
		float angle = 3.14159265f + yaw;
		layer.pose.orientation = {
			0.0f,
			sinf(angle * 0.5f),
			0.0f,
			cosf(angle * 0.5f)
		};
	} else {
		// Fallback: default position facing -Z
		layer.pose.position = { 0.0f, 1.0f, -0.80f };
		layer.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
	}

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

		// Warm white beam — semi-transparent
		uint8_t cr = 255, cg = 240, cb = 220, ca = 180;
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
		laserLayer[i].space = xr_gbl->floorSpace;
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
	cursorPos = (int)text.size();
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

static inline float xr_dot(const XrVector3f& a, const XrVector3f& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Compute a quaternion that orients a quad so its Y axis aligns with beamDir
// and the quad faces the viewer at viewerPos.
static XrQuaternionf beamOrientation(XrVector3f beamDir, XrVector3f midpoint, XrVector3f viewerPos)
{
	// up = beamDir (height axis of quad)
	XrVector3f up = beamDir;

	// forward = from midpoint toward viewer
	XrVector3f toViewer = {
		viewerPos.x - midpoint.x,
		viewerPos.y - midpoint.y,
		viewerPos.z - midpoint.z
	};
	float ml = sqrtf(toViewer.x * toViewer.x + toViewer.y * toViewer.y + toViewer.z * toViewer.z);
	XrVector3f fwd;
	if (ml > 0.001f) {
		fwd = { toViewer.x / ml, toViewer.y / ml, toViewer.z / ml };
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
	laserLayer[side].pose.orientation = beamOrientation(dir, mid, headWorldPos);
}

const std::vector<XrCompositionLayerBaseHeader*>& VRKeyboard::Update()
{
	activeLayers.clear();

	BaseSystem* sys = GetUnsafeBaseSystem();
	if (sys) {
		float time = (float)(GetTickCount64() / 1000.0);

		// Update head position for beam billboard orientation
		if (headLocked) {
			// In viewSpace the head IS the origin
			headWorldPos = { 0, 0, 0 };
		} else {
			XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
			if (XR_SUCCEEDED(xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace,
			        xr_gbl->GetBestTime(), &headLoc))
			    && (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
				headWorldPos = headLoc.pose.position;
			}
		}

		// Laser pointer hit testing
		bool anyLaserActive = false;
		for (int side = 0; side < 2; side++) {
			int hitResult = HitTestLaser(side);
			if (hitResult >= 0 && hitResult != selected[side]) {
				selected[side] = hitResult;
				dirty = true;
			}
			if (laserActive[side]) {
				anyLaserActive = true;
				UpdateLaserBeam(side);
			}
		}

		if (anyLaserActive)
			dirty = true;

		// Force redraw every 500ms for blinking cursor in non-minimal mode
		if (!minimal) {
			bool cursorBlink = ((GetTickCount64() / 500) % 2) == 0;
			static bool lastCursorBlink = false;
			if (cursorBlink != lastCursorBlink) {
				lastCursorBlink = cursorBlink;
				dirty = true;
			}
		}

		// Get controller states once for grab logic and input handling
		vr::VRControllerState_t states[2] = {};
		bool hasState[2] = { false, false };
		hasState[0] = sys->GetControllerState(1, &states[0], sizeof(states[0]));
		hasState[1] = sys->GetControllerState(2, &states[1], sizeof(states[1]));

		// Grab bar logic — trigger to drag, toggle to switch head-lock mode
		for (int side = 0; side < 2; side++) {
			if (!hasState[side])
				continue;

			bool trigNow = (states[side].ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
			bool trigJustPressed = trigNow && !lastTriggerState[side];
			bool trigJustReleased = !trigNow && lastTriggerState[side];
			lastTriggerState[side] = trigNow;

			// Head-lock toggle — trigger press on the toggle button
			if (trigJustPressed && laserOnToggle[side] && !grabActive) {
				headLocked = !headLocked;
				if (headLocked) {
					// Switch to head-locked mode
					grabActive = false;
					grabbingSide = -1;
					layer.space = xr_gbl->viewSpace;
					layer.pose.position = { 0.0f, -0.35f, -0.80f };
					layer.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
					for (int i = 0; i < 2; i++)
						laserLayer[i].space = xr_gbl->viewSpace;
					headWorldPos = { 0, 0, 0 };
				} else {
					// Switch to world-anchored — spawn at current head position
					layer.space = xr_gbl->floorSpace;
					for (int i = 0; i < 2; i++)
						laserLayer[i].space = xr_gbl->floorSpace;
					XrSpaceLocation hl = { XR_TYPE_SPACE_LOCATION };
					if (XR_SUCCEEDED(xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace,
					        xr_gbl->GetBestTime(), &hl))
					    && (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
					    && (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
						XrVector3f headFwd;
						XrVector3f localFwd = { 0.0f, 0.0f, -1.0f };
						rotate_vector_by_quaternion(localFwd, hl.pose.orientation, headFwd);
						headFwd.y = 0;
						float fwdLen = sqrtf(headFwd.x * headFwd.x + headFwd.z * headFwd.z);
						if (fwdLen > 0.001f) { headFwd.x /= fwdLen; headFwd.z /= fwdLen; }
						else { headFwd = { 0.0f, 0.0f, -1.0f }; }
						layer.pose.position = {
							hl.pose.position.x + headFwd.x * 0.80f,
							hl.pose.position.y - 0.35f,
							hl.pose.position.z + headFwd.z * 0.80f
						};
						float yaw = atan2f(headFwd.x, headFwd.z);
						float angle = 3.14159265f + yaw;
						layer.pose.orientation = {
							0.0f, sinf(angle * 0.5f), 0.0f, cosf(angle * 0.5f)
						};
					}
					headWorldPos = hl.pose.position;
				}
				dirty = true;
				continue; // don't also start a grab this frame
			}

			// Text bar click — position the text cursor
			if (trigJustPressed && laserOnTextBar[side] && !minimal) {
				int clickTexX = (int)(laserU[side] * texWidth);
				int BORD = 3;
				int pad = 8;
				int spaceW = font->Width(L' ');
				int textStartX = pad + BORD + 6 + spaceW; // matches Refresh() cursor origin
				int relX = clickTexX - textStartX;

				// Walk through text characters to find nearest boundary
				int accumX = 0;
				int newPos = 0;
				for (int i = 0; i < (int)text.size(); i++) {
					int charW = font->Width(text[i]);
					if (relX < accumX + charW / 2)
						break;
					accumX += charW;
					newPos = i + 1;
				}
				cursorPos = newPos;
				dirty = true;
				continue;
			}

			// Grab — trigger on drag area to reposition (only in world mode)
			// Uses laser ray-plane intersection so the keyboard follows the laser 1:1.
			if (trigJustPressed && laserOnGrabBar[side] && !grabActive && !headLocked) {
				// The laser already hit the keyboard — use that hit point
				if (laserActive[side]) {
					grabActive = true;
					grabbingSide = side;
					grabPlaneOrigin = layer.pose.position;
					// Offset from hit point to keyboard center
					grabOffset = {
						layer.pose.position.x - laserHitPoint[side].x,
						layer.pose.position.y - laserHitPoint[side].y,
						layer.pose.position.z - laserHitPoint[side].z
					};
				}
			}

			if (grabActive && grabbingSide == side) {
				if (trigJustReleased) {
					grabActive = false;
					grabbingSide = -1;
				} else if (trigNow) {
					// Intersect the laser ray with the ORIGINAL grab plane
					// (not the current keyboard position — avoids feedback lag)
					std::shared_ptr<BaseInput> input = GetBaseInput();
					if (input && input->AreActionsLoaded()) {
						XrSpace aimSpace = XR_NULL_HANDLE;
						input->GetHandSpace((vr::TrackedDeviceIndex_t)(side + 1), aimSpace, true);
						if (aimSpace != XR_NULL_HANDLE) {
							XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
							if (XR_SUCCEEDED(xrLocateSpace(aimSpace, layer.space,
							        xr_gbl->GetBestTime(), &loc))
							    && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
							    && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
								XrVector3f rayOrig = loc.pose.position;
								XrVector3f rayFwd = { 0, 0, -1 };
								XrVector3f rayDir;
								rotate_vector_by_quaternion(rayFwd, loc.pose.orientation, rayDir);

								XrVector3f planeN;
								rotate_vector_by_quaternion({ 0, 0, 1 }, layer.pose.orientation, planeN);
								float d = xr_dot(rayDir, planeN);
								if (fabsf(d) > 1e-6f) {
									XrVector3f PO = {
										grabPlaneOrigin.x - rayOrig.x,
										grabPlaneOrigin.y - rayOrig.y,
										grabPlaneOrigin.z - rayOrig.z
									};
									float t = xr_dot(PO, planeN) / d;
									if (t > 0.0f) {
										XrVector3f hit = {
											rayOrig.x + t * rayDir.x,
											rayOrig.y + t * rayDir.y,
											rayOrig.z + t * rayDir.z
										};
										layer.pose.position = {
											hit.x + grabOffset.x,
											hit.y + grabOffset.y,
											hit.z + grabOffset.z
										};
									}
								}
							}
						}
					}
				}
			}
		}

		// Controller input (trigger for typing)
		for (int side = 0; side < 2; side++) {
			if (!hasState[side])
				continue;
			// InjectThumbstickAsDpad(states[side]); // Disabled — laser pointers handle selection now
			HandleOverlayInput(side == 0 ? vr::Eye_Left : vr::Eye_Right, states[side], time);
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

// Returns: key ID (>= 0), -1 (miss), -2 (grab bar drag area), -3 (toggle button)
int VRKeyboard::HitTestLaser(int side)
{
	laserActive[side] = false;
	laserOnGrabBar[side] = false;
	laserOnToggle[side] = false;
	laserOnTextBar[side] = false;

	std::shared_ptr<BaseInput> input = GetBaseInput();
	if (!input || !input->AreActionsLoaded())
		return -1;

	XrSpace aimSpace = XR_NULL_HANDLE;
	input->GetHandSpace((vr::TrackedDeviceIndex_t)(side + 1), aimSpace, true);
	if (aimSpace == XR_NULL_HANDLE)
		return -1;

	// Locate controller in the keyboard's reference space (viewSpace or floorSpace)
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	XrResult result = xrLocateSpace(aimSpace, layer.space,
	    xr_gbl->GetBestTime(), &location);

	if (XR_FAILED(result)
	    || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
	    || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return -1;

	XrVector3f rayOrigin = location.pose.position;
	XrVector3f fwd = { 0.0f, 0.0f, -1.0f };
	XrVector3f rayDir;
	rotate_vector_by_quaternion(fwd, location.pose.orientation, rayDir);

	// Oriented plane intersection — keyboard can face any direction in world space
	XrVector3f kbCenter = layer.pose.position;
	XrVector3f planeNormal, localRight, localUp;
	rotate_vector_by_quaternion({ 0, 0, 1 }, layer.pose.orientation, planeNormal);
	rotate_vector_by_quaternion({ 1, 0, 0 }, layer.pose.orientation, localRight);
	rotate_vector_by_quaternion({ 0, 1, 0 }, layer.pose.orientation, localUp);

	float denom = xr_dot(rayDir, planeNormal);
	if (fabsf(denom) < 1e-6f)
		return -1; // ray parallel to keyboard plane

	XrVector3f PO = { kbCenter.x - rayOrigin.x, kbCenter.y - rayOrigin.y, kbCenter.z - rayOrigin.z };
	float t = xr_dot(PO, planeNormal) / denom;
	if (t <= 0.0f)
		return -1; // intersection behind the ray

	XrVector3f hitPoint = {
		rayOrigin.x + t * rayDir.x,
		rayOrigin.y + t * rayDir.y,
		rayOrigin.z + t * rayDir.z
	};

	// Project hit onto keyboard local axes for UV coordinates
	XrVector3f HP = { hitPoint.x - kbCenter.x, hitPoint.y - kbCenter.y, hitPoint.z - kbCenter.z };
	float localXCoord = xr_dot(HP, localRight);
	float localYCoord = xr_dot(HP, localUp);

	float u = (localXCoord + layer.size.width * 0.5f) / layer.size.width;
	float v = (localYCoord + layer.size.height * 0.5f) / layer.size.height;

	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return -1; // missed the quad

	// Store laser data for beam rendering and cursor dot
	laserActive[side] = true;
	laserOrigin[side] = rayOrigin;
	laserHitPoint[side] = hitPoint;
	laserU[side] = u;
	laserV[side] = v;

	// Convert to pixel coordinates (texture Y is flipped: top of quad = top of texture)
	int texX = (int)(u * texWidth);
	int texY = (int)((1.0f - v) * texHeight);

	// Check grab bar region (top strip of texture)
	if (texY < GRAB_BAR_HEIGHT) {
		// Toggle button on the right side of the grab bar
		int toggleLeft = (int)texWidth - TOGGLE_BTN_WIDTH - 8;
		if (texX >= toggleLeft) {
			laserOnToggle[side] = true;
			return -3; // toggle button hit
		}
		laserOnGrabBar[side] = true;
		return -2; // grab bar drag area
	}

	// Hit-test text input bar (only in non-minimal mode)
	int padding = 8;
	int kbWidth = layout->GetWidth();
	int keySize = (((int)texWidth - padding) / kbWidth) - padding;
	if (!minimal) {
		int textBarY = GRAB_BAR_HEIGHT + padding;
		int textBarH = keySize;
		if (texY >= textBarY && texY < textBarY + textBarH
		    && texX >= padding && texX < (int)texWidth - padding) {
			laserOnTextBar[side] = true;
			return -4; // text bar hit
		}
	}

	// Hit-test against keyboard keys (shifted down by grab bar height)
	int keyAreaBaseY = (minimal ? padding : padding + keySize + padding) + GRAB_BAR_HEIGHT;

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
	// DPad navigation disabled — laser pointers handle selection now
	// GET_BTTN(left, k_EButton_DPad_Left);
	// GET_BTTN(right, k_EButton_DPad_Right);
	// GET_BTTN(up, k_EButton_DPad_Up);
	// GET_BTTN(down, k_EButton_DPad_Down);
	GET_BTTN_LAST(trigger, k_EButton_SteamVR_Trigger);
	GET_BTTN_LAST(grip, k_EButton_Grip);
#undef GET_BTTN
#undef GET_BTTN_LAST

	if (grip && !grip_last && !grabActive) {
		closed = true;
		SubmitEvent(VREvent_KeyboardClosed, 0);
		return;
	}

	const KeyboardLayout::Key& key = layout->GetKeymap()[selected[side]];

	// Don't fire key presses when laser is on the grab bar, toggle button, or text bar
	if (trigger && !trigger_last
	    && !laserOnGrabBar[(int)side] && !laserOnToggle[(int)side] && !laserOnTextBar[(int)side]) {
		wchar_t ch = caseMode == ECaseMode::LOWER ? key.ch : key.shift;

		bool submitKeyEvent = false;

		if (ch == '\x01' || ch == '\x02') {
			// Shift
			ECaseMode target = ch == '\x02' ? ECaseMode::LOCK : ECaseMode::SHIFT;
			caseMode = caseMode == target ? ECaseMode::LOWER : target;
		} else if (ch == '\b') {
			// Backspace — delete character before cursor
			if (cursorPos > 0 && !text.empty()) {
				text.erase(cursorPos - 1, 1);
				cursorPos--;
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
			text.insert(cursorPos, 1, ch);
			cursorPos++;

			submitKeyEvent = true;

			if (caseMode == ECaseMode::SHIFT)
				caseMode = ECaseMode::LOWER;
		}

		if (submitKeyEvent) {
			SubmitEvent(VREvent_KeyboardCharInput, minimal ? ch : 0);
		}

		dirty = true;
	}

	// DPad movement disabled — laser pointers handle selection now
	// Kept for future reference:
	/*
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
	*/
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

	// Grab bar at top — slightly lighter strip
	fillArea(BORD, BORD, desc.Width - BORD * 2, GRAB_BAR_HEIGHT - BORD, 55, 48, 40);
	// Golden separator at bottom of grab bar
	fillArea(BORD, GRAB_BAR_HEIGHT - 2, desc.Width - BORD * 2, 2, 140, 115, 45);

	// MOVE and LOCK buttons in the grab bar
	{
		int btnY = BORD + 5;
		int btnH = GRAB_BAR_HEIGHT - BORD - 9;
		// Vertically center text in the button using actual font metrics
		int fontH = (int)font->GetLineHeight();
		int textYOff = (btnH - fontH) / 2 + 4;

		// Check if any laser is hovering over the drag area
		bool moveHover = (!headLocked && (laserOnGrabBar[0] || laserOnGrabBar[1]));
		bool lockHover = (laserOnToggle[0] || laserOnToggle[1]);

		// ── MOVE button ──
		int moveTextW = font->Width(L"MOVE");
		int moveBtnW = moveTextW + 24;
		int moveBtnX = 12;
		fillArea(moveBtnX, btnY, moveBtnW, btnH, 140, 115, 45); // golden border
		if (moveHover) {
			fillArea(moveBtnX + 2, btnY + 2, moveBtnW - 4, btnH - 4, 120, 100, 40); // lit up
		} else if (headLocked) {
			fillArea(moveBtnX + 2, btnY + 2, moveBtnW - 4, btnH - 4, 38, 33, 28); // dark (disabled)
		} else {
			fillArea(moveBtnX + 2, btnY + 2, moveBtnW - 4, btnH - 4, 50, 44, 36); // normal
		}
		pix_t moveColour = moveHover
		    ? pix_t{ 240, 220, 160, 255 }  // Bright gold on hover
		    : headLocked
		        ? pix_t{ 70, 60, 45, 255 }  // Dim when head-locked
		        : pix_t{ 180, 155, 75, 255 }; // Golden
		print(moveBtnX + (moveBtnW - moveTextW) / 2, btnY + textYOff, moveColour, L"MOVE", false);

		// ── LOCK button ──
		int lockBtnX = (int)desc.Width - TOGGLE_BTN_WIDTH - 8;
		int lockBtnW = TOGGLE_BTN_WIDTH;
		fillArea(lockBtnX, btnY, lockBtnW, btnH, 140, 115, 45); // golden border
		if (headLocked) {
			fillArea(lockBtnX + 2, btnY + 2, lockBtnW - 4, btnH - 4, 120, 100, 40); // active
		} else if (lockHover) {
			fillArea(lockBtnX + 2, btnY + 2, lockBtnW - 4, btnH - 4, 65, 56, 42); // hover hint
		} else {
			fillArea(lockBtnX + 2, btnY + 2, lockBtnW - 4, btnH - 4, 42, 37, 30); // inactive
		}
		pix_t lockColour = headLocked
		    ? pix_t{ 240, 220, 160, 255 }
		    : lockHover
		        ? pix_t{ 200, 180, 120, 255 }
		        : pix_t{ 130, 110, 70, 255 };
		int lockTextW = font->Width(L"LOCK");
		print(lockBtnX + (lockBtnW - lockTextW) / 2, btnY + textYOff, lockColour, L"LOCK", false);
	}

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

		if (leftSel || rightSel) {
			// Cream gold highlight for selected key
			fillArea(x + BORD, y + BORD, width - BORD * 2, height - BORD * 2, 95, 80, 40);
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

	int keyAreaBaseY = (minimal ? padding : padding + keySize + padding) + GRAB_BAR_HEIGHT;
	for (const KeyboardLayout::Key& key : layout->GetKeymap()) {
		int x = padding + (int)((keySize + padding) * key.x);
		int y = keyAreaBaseY + (int)((keySize + padding) * key.y);

		drawKey(x, y, key);
	}

	if (!minimal) {
		// Text input bar — golden border with dark interior (shifted below grab bar)
		int textBarY = GRAB_BAR_HEIGHT + padding;
		fillArea(padding, textBarY, desc.Width - padding * 2, keySize, 140, 115, 45);
		fillArea(padding + BORD, textBarY + BORD, desc.Width - padding * 2 - BORD * 2, keySize - BORD * 2, 38, 33, 28);

		pix_t targetColour = { 220, 200, 155, 255 }; // Parchment text

		print(padding + BORD + 6, textBarY + BORD + 4, targetColour, text);

		// Blinking text cursor — 500ms on, 500ms off
		bool cursorVisible = ((GetTickCount64() / 500) % 2) == 0;
		if (cursorVisible) {
			// Calculate cursor X position by measuring text up to cursorPos
			// The +spaceW offset accounts for the font's XOffset shifting glyphs right
			int spaceW = font->Width(L' ');
			int cursorX = padding + BORD + 6 + spaceW;
			for (int i = 0; i < cursorPos && i < (int)text.size(); i++)
				cursorX += font->Width(text[i]);

			int cursorY = textBarY + BORD + 2;
			int cursorH = keySize - BORD * 2 - 4;
			pix_t cursorColour = { 220, 200, 155, 255 }; // Same parchment color as text
			fillArea(cursorX, cursorY, 2, cursorH, cursorColour.r, cursorColour.g, cursorColour.b);
		}
	}

	// Draw laser cursor dots on the keyboard surface
	for (int side = 0; side < 2; side++) {
		if (!laserActive[side])
			continue;

		int cx = (int)(laserU[side] * desc.Width);
		int cy = (int)((1.0f - laserV[side]) * desc.Height);
		int radius = 6;

		// Warm white cursor dot
		int cr = 255, cg = 240, cb = 220;

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
