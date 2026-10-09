#include <ESPER/SchemaTypes.h>

#include <ESPER/generated/Tables.h>

namespace esper::schema {

const SchemaSet& Main() { return tables::kSchema; }

} // namespace esper::schema
