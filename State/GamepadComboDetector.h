#pragma once

class GamepadComboDetector {
  public:
	struct ComboResult {
		Windows::Gaming::Input::GamepadReading currentReading; // untouched reading
		Windows::Gaming::Input::GamepadReading maskedReading;  // reading with pending combo buttons masked out
		bool comboTriggered;                                   // True when combo completes
	};

	GamepadComboDetector() = default;

	ComboResult GetComboResult(int gamepadIndex, int comboTimeout);
	void Reset();

  private:
	enum class ComboState {
		None,
		ViewWaiting,
		MenuWaiting,
		ComboActive
	};

	// Combo state tracking
	bool viewPressed = false;
	bool menuPressed = false;
	int64_t comboStartTime = 0;
	ComboState state = ComboState::None;
};
