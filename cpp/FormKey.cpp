#include <ESPER/FormKey.h>
#include <cstdio>

namespace esper {

std::string FormKey::ToString() const {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06X", localId & 0x00FFFFFFu);
    return std::string(buf) + ":" + plugin;
}

} // namespace esper
