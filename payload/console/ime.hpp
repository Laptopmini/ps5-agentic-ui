// The PS5 system keyboard (sceImeDialog), opened directly rather than through SDL so it can be
// centred and given a title, a placeholder and password mode.
//
// SDL's PS5 driver still polls the dialog: when it finishes with OK, SDL's pump sends a Return key
// (and terminates the dialog). So: open(), then each frame feed the Return key to sawReturn() and
// call poll() after the events; the text is read from our own buffer.
#pragma once

#include <string>

namespace ime {

struct Request {
  std::string title, placeholder;
  bool password = false;
  bool send = false;  // Enter key labelled "Send" (the message bar)
};

/** Open the keyboard. Returns 0, or the sceImeDialogInit error. */
int open(const Request& request);

enum class State { Idle, Open, Done, Cancelled };

/** Note that SDL reported a Return key while the keyboard was open (it closed with OK). */
void sawReturn();
/** Call once per frame after handling events. Done/Cancelled is reported once, then Idle. */
State poll();
/** The text typed, as UTF-8 (valid after Done). */
std::string text();

}  // namespace ime
