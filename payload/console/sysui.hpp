// The PS5's own UI for what the agent shows: the system message dialog (sceMsgDialog, the
// confirm games use) and system notifications. The dialog libraries are loaded at runtime
// (dlopen), since the payload SDK has no link stubs for them.
#pragma once

#include <string>
#include <vector>

namespace sysui {

/** Load and initialise the dialog libraries. False (with a reason) if they're unavailable. */
bool init(std::string& why);

struct Confirm {
  std::string message, confirmLabel, cancelLabel;
  bool destructive = false;
};
/** Open the system message dialog. Returns 0, or the error from the console. */
int openConfirm(const Confirm& c, std::string& how);

enum class State { Idle, Open, Confirmed, Cancelled };
/** Call once per frame. Confirmed/Cancelled is reported once, then Idle. */
State poll();
/** Close the dialog if it's open (the agent no longer needs an answer). */
void close();

/** The system progress dialog (message dialog in progress-bar mode) for a running tool. */
int openProgress(const std::string& label);
/** 0 to 1. */
void setProgress(float value);
bool progressOpen();
void closeProgress();

/** A system notification (toast). icon: info, success, warning, error… (only shapes the text). */
void notify(const std::string& text, const std::string& icon);

/** What the system says about the app (sceSystemServiceGetStatus), and events it sent. */
struct SystemState {
  int status = 0;  // the call's result (0 = OK)
  bool overlaid = false;    // a system UI (the PS button's Control Center, a dialog) is over the app
  bool background = false;  // the app runs in the background (the Home screen is showing)
  std::vector<unsigned> events;  // sceSystemServiceReceiveEvent types received by this call
};
/** Ask the system; call a few times a second. */
SystemState pollSystem();

}  // namespace sysui
