#include "guid.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

void FormatValue(TStringBuilderBase* builder, TGuid value, TStringBuf /*format*/)
{
    char* begin = builder->Preallocate(MaxGuidStringSize);
    char* end = WriteGuidToBuffer(begin, value);
    builder->Advance(end - begin);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

