#include "pch.h"
#include "GamepadComboDetector.h"
#include "Utils.hpp"

using Windows::Gaming::Input::Gamepad;
using Windows::Gaming::Input::GamepadButtons;
using Windows::Gaming::Input::GamepadReading;

using namespace moonlight_xbox_dx;

static inline bool isPressed(GamepadButtons buttons, GamepadButtons b) {
	return (buttons & b) == b;
}

static inline GamepadButtons clearButtons(GamepadButtons buttons, GamepadButtons mask) {
	return static_cast<GamepadButtons>(
	    static_cast<uint32_t>(buttons) & ~static_cast<uint32_t>(mask));
}

static inline GamepadButtons setButtons(GamepadButtons buttons, GamepadButtons mask) {
	return static_cast<GamepadButtons>(
		static_cast<uint32_t>(buttons) | static_cast<uint32_t>(mask));
}

static inline GamepadReading EmptyReading() {
	static GamepadReading r = [] {
		static GamepadReading er{};
		ZeroMemory(&er, sizeof(GamepadReading));
		return er;
	}();
	return r;
}

GamepadComboDetector::ComboResult GamepadComboDetector::GetComboResult(int gamepadIndex, int comboTimeoutMs = 125) {
	ComboResult result{};
	result.comboTriggered = false;
	result.currentReading = EmptyReading();
	result.maskedReading = EmptyReading();

	auto gamepads = Gamepad::Gamepads;
	if (gamepads == nullptr || gamepadIndex < 0 || gamepadIndex >= static_cast<int>(gamepads->Size)) {
		// Return a default result. Caller can decide what to do.
		return result;
	}

	Gamepad^ gamepad = gamepads->GetAt(gamepadIndex);
	if (gamepad == nullptr) {
		return result;
	}

	result.currentReading = gamepad->GetCurrentReading();
	GamepadButtons buttons = result.currentReading.Buttons;

	const bool viewCurrentlyPressed = isPressed(buttons, GamepadButtons::View);
	const bool menuCurrentlyPressed = isPressed(buttons, GamepadButtons::Menu);

	// Start with unmasked buttons
	GamepadButtons maskedButtons = buttons;

	switch (state) {
	case ComboState::None:
		// Check if either button is newly pressed
		if (viewCurrentlyPressed && !viewPressed) {
			state = ComboState::ViewWaiting;
			comboStartTime = QpcNow();
			maskedButtons = clearButtons(buttons, GamepadButtons::View);
			//Utils::Logf("view: waiting...\n");
		} else if (menuCurrentlyPressed && !menuPressed) {
			state = ComboState::MenuWaiting;
			comboStartTime = QpcNow();
			maskedButtons = clearButtons(buttons, GamepadButtons::Menu);
			//Utils::Logf("menu: waiting...\n");
		}
		break;

	case ComboState::ViewWaiting:
		// Check timeout
		if (QpcToMs(QpcNow() - comboStartTime) > comboTimeoutMs) {
			state = ComboState::None;
			//Utils::Logf("view: timed out waiting, sending to host\n");
			maskedButtons = setButtons(buttons, GamepadButtons::View);
			break;
		}

		// Check if View was released before combo completed
		if (!viewCurrentlyPressed) {
			state = ComboState::None;
			//Utils::Logf("view: released, sending to host\n");
			maskedButtons = setButtons(buttons, GamepadButtons::View);
			break;
		}

		// Mask View while waiting
		maskedButtons = clearButtons(buttons, GamepadButtons::View);

		// Check if Menu is now pressed (combo complete)
		if (menuCurrentlyPressed && !menuPressed) {
			state = ComboState::ComboActive;
			result.comboTriggered = true;
			maskedButtons = clearButtons(buttons, GamepadButtons::View | GamepadButtons::Menu);
			//Utils::Logf("view: menu pressed, combo active\n");
		}
		break;

	case ComboState::MenuWaiting:
		if (QpcToMs(QpcNow() - comboStartTime) > comboTimeoutMs) {
			state = ComboState::None;
			//Utils::Logf("menu: timed out waiting, sending to host\n");
			maskedButtons = setButtons(buttons, GamepadButtons::Menu);
			break;
		}

		if (!menuCurrentlyPressed) {
			state = ComboState::None;
			//Utils::Logf("menu: released, sending to host\n");
			maskedButtons = setButtons(buttons, GamepadButtons::Menu);
			break;
		}

		maskedButtons = clearButtons(buttons, GamepadButtons::Menu);

		if (viewCurrentlyPressed && !viewPressed) {
			state = ComboState::ComboActive;
			result.comboTriggered = true;
			maskedButtons = clearButtons(buttons, GamepadButtons::View | GamepadButtons::Menu);
			//Utils::Logf("menu: view pressed, combo active\n");
		}
		break;

	case ComboState::ComboActive:
		// Remain in combo state while both are held
		if (viewCurrentlyPressed && menuCurrentlyPressed) {
			// Continue masking both buttons
			maskedButtons = clearButtons(buttons, GamepadButtons::View | GamepadButtons::Menu);
		} else {
			// One or both released, reset state
			state = ComboState::None;
			//Utils::Logf("menu+view: combo done\n");
		}
		break;
	}

	viewPressed = viewCurrentlyPressed;
	menuPressed = menuCurrentlyPressed;
	result.maskedReading = result.currentReading;
	result.maskedReading.Buttons = maskedButtons;

	return result;
}

void GamepadComboDetector::Reset() {
	state = ComboState::None;
	viewPressed = false;
	menuPressed = false;
}
