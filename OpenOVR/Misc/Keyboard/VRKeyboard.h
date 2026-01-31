#pragma once

#include <d3d11.h>

#include <codecvt>
#include <functional>
#include <locale>
#include <memory>
#include <string>
#include <vector>

#include "Misc/xr_ext.h"
#include "Misc/xrutil.h"

#include "KeyboardLayout.h"
#include "SudoFontMeta.h"

class VRKeyboard {
public:
	typedef std::function<void(vr::VREvent_t)> eventDispatch_t;

	// Yay this isn't in vrtypes.h
	// Maybe we should make the header splitter put enums somewhere else?
	enum EGamepadTextInputMode {
		k_EGamepadTextInputModeNormal = 0,
		k_EGamepadTextInputModePassword = 1,
		k_EGamepadTextInputModeSubmit = 2,
	};

	VRKeyboard(ID3D11Device* dev, uint64_t userValue, uint32_t maxLength, bool minimal, eventDispatch_t dispatch, EGamepadTextInputMode inputMode);
	~VRKeyboard();

	std::wstring contents();
	void contents(std::wstring);

	const std::vector<XrCompositionLayerBaseHeader*>& Update();

	void HandleOverlayInput(vr::EVREye controllerDeviceIndex, vr::VRControllerState_t state, float time);

	enum ECaseMode {
		LOWER,
		SHIFT,
		LOCK,
	};

	static std::wstring_convert<std::codecvt_utf8<wchar_t>> CHAR_CONV;

	bool IsClosed() { return closed; }

	void SetTransform(vr::HmdMatrix34_t transform);

private:
	ID3D11Device* const dev;
	ID3D11DeviceContext* ctx;

	bool dirty = true;
	bool closed = false;

	std::wstring text;
	int cursorPos = 0; // insertion point: 0 = before first char, text.size() = after last
	ECaseMode caseMode = LOWER;

	uint64_t userValue; // Arbitary user data, to be passed into the SteamVR events
	uint32_t maxLength;
	bool minimal;
	eventDispatch_t eventDispatch;
	EGamepadTextInputMode inputMode;

	// OpenXR swap chain and composition layer
	XrSwapchain chain = XR_NULL_HANDLE;
	uint32_t texWidth = 1024;
	uint32_t texHeight = 478;
	XrCompositionLayerQuad layer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	std::vector<XrSwapchainImageD3D11KHR> swapchainImages;

	std::unique_ptr<SudoFontMeta> font;
	std::unique_ptr<KeyboardLayout> layout;

	// These use the OpenVR eye constants
	float lastInputTime[2];
	int repeatCount[2];
	int selected[2];
	uint64_t lastButtonState[2];

	// Grab bar — trigger on the top strip to grab and reposition the keyboard
	static constexpr int GRAB_BAR_HEIGHT = 52; // pixels at top of texture
	static constexpr int TOGGLE_BTN_WIDTH = 120; // headlock toggle button width
	bool grabActive = false;
	int grabbingSide = -1;
	XrVector3f grabOffset = {};      // offset from laser hit to keyboard center
	XrVector3f grabPlaneOrigin = {}; // keyboard center when grab started (reference plane)
	bool lastTriggerState[2] = { false, false };
	bool laserOnGrabBar[2] = { false, false };
	bool laserOnToggle[2] = { false, false };
	bool laserOnTextBar[2] = { false, false };
	bool headLocked = false; // true = head-locked (viewSpace), false = world-anchored (floorSpace)
	XrVector3f headWorldPos = {}; // Head position in world/view space, updated each frame

	// Laser pointer data
	bool laserActive[2] = { false, false };
	float laserU[2] = {};
	float laserV[2] = {};
	XrVector3f laserOrigin[2] = {};
	XrVector3f laserHitPoint[2] = {};

	// Laser beam composition layers (one per hand)
	XrSwapchain laserChain[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrCompositionLayerQuad laserLayer[2] = {};

	// All layers returned by Update (keyboard + laser beams)
	std::vector<XrCompositionLayerBaseHeader*> activeLayers;

	void Refresh();

	void SubmitEvent(vr::EVREventType ev, wchar_t ch);

	int HitTestLaser(int side);
	void UpdateLaserBeam(int side);
};
