#include "wecker_presence.h"

#ifndef WLED_DISABLE_WECKER
// Statische Member-Definition gehört in genau EINE Übersetzungseinheit (ODR),
// wie bei allen anderen Usermods (_name im .cpp, nicht im Header).
const char WeckerPresenceUsermod::_name[] PROGMEM = "wecker_presence";

WeckerPresenceUsermod weckerPresence;
REGISTER_USERMOD(weckerPresence);
#endif // WLED_DISABLE_WECKER
