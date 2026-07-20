#include "wecker_presence.h"

#ifndef WLED_DISABLE_WECKER
const char WeckerPresenceUsermod::_name[] PROGMEM = "wecker_presence";

WeckerPresenceUsermod weckerPresence;
REGISTER_USERMOD(weckerPresence);
#endif // WLED_DISABLE_WECKER
