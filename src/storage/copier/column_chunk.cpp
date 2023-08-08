#include "storage/copier/column_chunk.h"

#include "storage/copier/string_column_chunk.h"
#include "storage/copier/struct_column_chunk.h"
#include "storage/copier/table_copy_utils.h"
#include "storage/copier/var_list_column_chunk.h"
#include "storage/storage_structure/storage_structure_utils.h"

using namespace kuzu::common;
using namespace kuzu::transaction;

namespace kuzu {
namespace storage {

ColumnChunk::ColumnChunk(LogicalType dataType, CopyDescription* copyDescription, bool hasNullChunk)
    : dataType{std::move(dataType)}, numBytesPerValue{getDataTypeSizeInChunk(this->dataType)},
      copyDescription{copyDescription} {
    if (hasNullChunk) {
        nullChunk = std::make_unique<NullColumnChunk>();
    }
}

void ColumnChunk::initialize(offset_t numValues) {
    numBytes = numBytesPerValue * numValues;
    buffer = std::make_unique<uint8_t[]>(numBytes);
    static_cast<ColumnChunk*>(nullChunk.get())->initialize(numValues);
}

void ColumnChunk::resetToEmpty() {
    if (nullChunk) {
        nullChunk->resetNullBuffer();
    }
}

void ColumnChunk::append(
    ValueVector* vector, offset_t startPosInChunk, uint32_t numValuesToAppend) {
    assert(vector->dataType.getLogicalTypeID() == LogicalTypeID::ARROW_COLUMN);
    auto array = ArrowColumnVector::getArrowColumn(vector).get();
    append(array, startPosInChunk, numValuesToAppend);
}

void ColumnChunk::append(ColumnChunk* other, offset_t startPosInOtherChunk,
    offset_t startPosInChunk, uint32_t numValuesToAppend) {
    if (nullChunk) {
        nullChunk->append(
            other->nullChunk.get(), startPosInOtherChunk, startPosInChunk, numValuesToAppend);
    }
    memcpy(buffer.get() + startPosInChunk * numBytesPerValue,
        other->buffer.get() + startPosInOtherChunk * numBytesPerValue,
        numValuesToAppend * numBytesPerValue);
}

void ColumnChunk::append(
    arrow::Array* array, offset_t startPosInChunk, uint32_t numValuesToAppend) {
    switch (array->type_id()) {
    case arrow::Type::BOOL: {
        templateCopyArrowArray<bool>(array, startPosInChunk, numValuesToAppend);
    } break;
    case arrow::Type::INT16: {
        templateCopyArrowArray<int16_t>(array, startPosInChunk, numValuesToAppend);
    } break;
    case arrow::Type::INT32: {
        templateCopyArrowArray<int32_t>(array, startPosInChunk, numValuesToAppend);
    } break;
    case arrow::Type::INT64: {
        templateCopyArrowArray<int64_t>(array, startPosInChunk, numValuesToAppend);
    } break;
    case arrow::Type::DOUBLE: {
        templateCopyArrowArray<double_t>(array, startPosInChunk, numValuesToAppend);
    } break;
    case arrow::Type::FLOAT: {
        templateCopyArrowArray<float_t>(array, startPosInChunk, numValuesToAppend);
    } break;
    case arrow::Type::DATE32: {
        templateCopyArrowArray<date_t>(array, startPosInChunk, numValuesToAppend);
    } break;
    case arrow::Type::TIMESTAMP: {
        templateCopyArrowArray<timestamp_t>(array, startPosInChunk, numValuesToAppend);
    } break;
    case arrow::Type::FIXED_SIZE_LIST: {
        templateCopyArrowArray<uint8_t*>(array, startPosInChunk, numValuesToAppend);
    } break;
    case arrow::Type::STRING: {
        switch (dataType.getLogicalTypeID()) {
        case LogicalTypeID::DATE: {
            templateCopyValuesAsString<date_t>(array, startPosInChunk, numValuesToAppend);
        } break;
        case LogicalTypeID::TIMESTAMP: {
            templateCopyValuesAsString<timestamp_t>(array, startPosInChunk, numValuesToAppend);
        } break;
        case LogicalTypeID::INTERVAL: {
            templateCopyValuesAsString<interval_t>(array, startPosInChunk, numValuesToAppend);
        } break;
        case LogicalTypeID::FIXED_LIST: {
            // Fixed list is a fixed-sized blob.
            templateCopyValuesAsString<uint8_t*>(array, startPosInChunk, numValuesToAppend);
        } break;
        default: {
            throw NotImplementedException("Unsupported ColumnChunk::append from arrow STRING");
        }
        }
    } break;
    default: {
        throw NotImplementedException("ColumnChunk::append");
    }
    }
}

void ColumnChunk::write(const Value& val, uint64_t posToWrite) {
    nullChunk->setNull(posToWrite, val.isNull());
    if (val.isNull()) {
        return;
    }
    switch (dataType.getPhysicalType()) {
    case PhysicalTypeID::BOOL: {
        setValue(val.getValue<bool>(), posToWrite);
    } break;
    case PhysicalTypeID::INT64: {
        setValue(val.getValue<int64_t>(), posToWrite);
    } break;
    case PhysicalTypeID::INT32: {
        setValue(val.getValue<int32_t>(), posToWrite);
    } break;
    case PhysicalTypeID::INT16: {
        setValue(val.getValue<int16_t>(), posToWrite);
    } break;
    case PhysicalTypeID::DOUBLE: {
        setValue(val.getValue<double_t>(), posToWrite);
    } break;
    case PhysicalTypeID::FLOAT: {
        setValue(val.getValue<float_t>(), posToWrite);
    } break;
    case PhysicalTypeID::INTERVAL: {
        setValue(val.getValue<interval_t>(), posToWrite);
    } break;
    case PhysicalTypeID::INTERNAL_ID: {
        setValue(val.getValue<internalID_t>(), posToWrite);
    } break;
    default: {
        throw NotImplementedException{"ColumnChunk::write"};
    }
    }
}

void ColumnChunk::resize(uint64_t numValues) {
    auto numBytesAfterResize = numValues * numBytesPerValue;
    auto resizedBuffer = std::make_unique<uint8_t[]>(numBytesAfterResize);
    memcpy(resizedBuffer.get(), buffer.get(), numBytes);
    numBytes = numBytesAfterResize;
    buffer = std::move(resizedBuffer);
    if (nullChunk) {
        nullChunk->resize(numValues);
    }
    for (auto& child : childrenChunks) {
        child->resize(numValues);
    }
}

template<typename T>
void ColumnChunk::templateCopyArrowArray(
    arrow::Array* array, offset_t startPosInChunk, uint32_t numValuesToAppend) {
    const auto& arrowArray = array->data();
    auto valuesInChunk = (T*)buffer.get();
    auto valuesInArray = arrowArray->GetValues<T>(1 /* value buffer */);
    if (arrowArray->MayHaveNulls()) {
        for (auto i = 0u; i < numValuesToAppend; i++) {
            auto posInChunk = startPosInChunk + i;
            if (arrowArray->IsNull(i)) {
                nullChunk->setNull(posInChunk, true);
                continue;
            }
            valuesInChunk[posInChunk] = valuesInArray[i];
        }
    } else {
        for (auto i = 0u; i < numValuesToAppend; i++) {
            auto posInChunk = startPosInChunk + i;
            valuesInChunk[posInChunk] = valuesInArray[i];
        }
    }
}

template<>
void ColumnChunk::templateCopyArrowArray<bool>(
    arrow::Array* array, offset_t startPosInChunk, uint32_t numValuesToAppend) {
    auto* boolArray = (arrow::BooleanArray*)array;
    auto data = boolArray->data();
    auto valuesInChunk = (bool*)(buffer.get());
    if (data->MayHaveNulls()) {
        for (auto i = 0u; i < numValuesToAppend; i++) {
            auto posInChunk = startPosInChunk + i;
            if (data->IsNull(i)) {
                nullChunk->setNull(posInChunk, true);
                continue;
            }
            valuesInChunk[posInChunk] = boolArray->Value(i);
        }
    } else {
        for (auto i = 0u; i < numValuesToAppend; i++) {
            auto posInChunk = startPosInChunk + i;
            valuesInChunk[posInChunk] = boolArray->Value(i);
        }
    }
}

template<>
void ColumnChunk::templateCopyArrowArray<uint8_t*>(
    arrow::Array* array, offset_t startPosInChunk, uint32_t numValuesToAppend) {
    auto fixedSizedListArray = (arrow::FixedSizeListArray*)array;
    auto valuesInList = (uint8_t*)fixedSizedListArray->values()->data()->buffers[1]->data();
    if (fixedSizedListArray->data()->MayHaveNulls()) {
        for (auto i = 0u; i < numValuesToAppend; i++) {
            auto posInChunk = startPosInChunk + i;
            if (fixedSizedListArray->data()->IsNull(i)) {
                nullChunk->setNull(posInChunk, true);
                continue;
            }
            auto posInList = fixedSizedListArray->offset() + i;
            memcpy(buffer.get() + getOffsetInBuffer(posInChunk),
                valuesInList + posInList * numBytesPerValue, numBytesPerValue);
        }
    } else {
        for (auto i = 0u; i < numValuesToAppend; i++) {
            auto posInChunk = startPosInChunk + i;
            auto posInList = fixedSizedListArray->offset() + i;
            memcpy(buffer.get() + getOffsetInBuffer(posInChunk),
                valuesInList + posInList * numBytesPerValue, numBytesPerValue);
        }
    }
}

template<typename T>
void ColumnChunk::templateCopyValuesAsString(
    arrow::Array* array, offset_t startPosInChunk, uint32_t numValuesToAppend) {
    auto stringArray = (arrow::StringArray*)array;
    auto arrayData = stringArray->data();
    if (arrayData->MayHaveNulls()) {
        for (auto i = 0u; i < numValuesToAppend; i++) {
            auto posInChunk = startPosInChunk + i;
            if (arrayData->IsNull(i)) {
                nullChunk->setNull(posInChunk, true);
                continue;
            }
            auto value = stringArray->GetView(i);
            setValueFromString<T>(value.data(), value.length(), posInChunk);
        }
    } else {
        for (auto i = 0u; i < numValuesToAppend; i++) {
            auto posInChunk = startPosInChunk + i;
            auto value = stringArray->GetView(i);
            setValueFromString<T>(value.data(), value.length(), posInChunk);
        }
    }
}

page_idx_t ColumnChunk::getNumPages() const {
    auto numPagesToFlush = getNumPagesForBuffer();
    if (nullChunk) {
        numPagesToFlush += nullChunk->getNumPages();
    }
    for (auto& child : childrenChunks) {
        numPagesToFlush += child->getNumPages();
    }
    return numPagesToFlush;
}

page_idx_t ColumnChunk::flushBuffer(BMFileHandle* dataFH, page_idx_t startPageIdx) {
    FileUtils::writeToFile(dataFH->getFileInfo(), buffer.get(), numBytes,
        startPageIdx * BufferPoolConstants::PAGE_4KB_SIZE);
    return getNumPagesForBuffer();
}

uint32_t ColumnChunk::getDataTypeSizeInChunk(LogicalType& dataType) {
    switch (dataType.getLogicalTypeID()) {
    case LogicalTypeID::STRUCT: {
        return 0;
    }
    case LogicalTypeID::STRING: {
        return sizeof(ku_string_t);
    }
    case LogicalTypeID::VAR_LIST: {
        return sizeof(offset_t);
    }
    case LogicalTypeID::INTERNAL_ID: {
        return sizeof(offset_t);
    }
    // This should never be used for Nulls,
    // which use a different way of calculating the buffer size
    // FIXME(bmwinger): Setting this to 0 breaks everything.
    // It's being used in NullNodeColumn, and maybe there are some functions
    // relying on it despite the value being meaningless for a null bitfield.
    case LogicalTypeID::NULL_: {
        return 1;
    }
    default: {
        return StorageUtils::getDataTypeSize(dataType);
    }
    }
}

// TODO(bmwinger): Eventually, to support bitpacked bools, all these functions will need to be
// updated to support values sizes of less than one byte.
// But for the moment, this is the only generic ColumnChunk function which is needed by
// NullColumnChunk, and it's invoked directly on the nullColumn, so we don't need dynamic dispatch
void NullColumnChunk::append(NullColumnChunk* other, offset_t startPosInOtherChunk,
    common::offset_t startPosInChunk, uint32_t numValuesToAppend) {
    NullMask::copyNullMask((uint64_t*)other->buffer.get(), startPosInOtherChunk,
        (uint64_t*)buffer.get(), startPosInChunk, numValuesToAppend);
}

void FixedListColumnChunk::append(ColumnChunk* other, offset_t startPosInOtherChunk,
    common::offset_t startPosInChunk, uint32_t numValuesToAppend) {
    auto otherChunk = (FixedListColumnChunk*)other;
    if (nullChunk) {
        nullChunk->append(
            otherChunk->nullChunk.get(), startPosInOtherChunk, startPosInChunk, numValuesToAppend);
    }
    // TODO(Guodong): This can be optimized to not copy one by one.
    for (auto i = 0u; i < numValuesToAppend; i++) {
        memcpy(buffer.get() + getOffsetInBuffer(startPosInChunk + i),
            otherChunk->buffer.get() + getOffsetInBuffer(startPosInOtherChunk + i),
            numBytesPerValue);
    }
}

void FixedListColumnChunk::write(const Value& fixedListVal, uint64_t posToWrite) {
    assert(fixedListVal.getDataType()->getPhysicalType() == PhysicalTypeID::FIXED_LIST);
    nullChunk->setNull(posToWrite, fixedListVal.isNull());
    if (fixedListVal.isNull()) {
        return;
    }
    auto numValues = NestedVal::getChildrenSize(&fixedListVal);
    auto childType = FixedListType::getChildType(fixedListVal.getDataType());
    auto numBytesPerValueInList = getDataTypeSizeInChunk(*childType);
    auto bufferToWrite = buffer.get() + posToWrite * numBytesPerValue;
    for (auto i = 0u; i < numValues; i++) {
        auto val = NestedVal::getChildVal(&fixedListVal, i);
        switch (childType->getPhysicalType()) {
        case PhysicalTypeID::INT64: {
            memcpy(bufferToWrite, &val->getValueReference<int64_t>(), numBytesPerValueInList);
        } break;
        case PhysicalTypeID::INT32: {
            memcpy(bufferToWrite, &val->getValueReference<int32_t>(), numBytesPerValueInList);
        } break;
        case PhysicalTypeID::INT16: {
            memcpy(bufferToWrite, &val->getValueReference<int16_t>(), numBytesPerValueInList);
        } break;
        case PhysicalTypeID::DOUBLE: {
            memcpy(bufferToWrite, &val->getValueReference<double_t>(), numBytesPerValueInList);
        } break;
        case PhysicalTypeID::FLOAT: {
            memcpy(bufferToWrite, &val->getValueReference<float_t>(), numBytesPerValueInList);
        } break;
        default: {
            throw NotImplementedException{"FixedListColumnChunk::write"};
        }
        }
        bufferToWrite += numBytesPerValueInList;
    }
}

std::unique_ptr<ColumnChunk> ColumnChunkFactory::createColumnChunk(
    const LogicalType& dataType, CopyDescription* copyDescription) {
    std::unique_ptr<ColumnChunk> chunk;
    switch (dataType.getPhysicalType()) {
    case PhysicalTypeID::BOOL:
    case PhysicalTypeID::INT64:
    case PhysicalTypeID::INT32:
    case PhysicalTypeID::INT16:
    case PhysicalTypeID::DOUBLE:
    case PhysicalTypeID::FLOAT:
    case PhysicalTypeID::INTERVAL:
        chunk = std::make_unique<ColumnChunk>(dataType, copyDescription);
        break;
    case PhysicalTypeID::FIXED_LIST:
        chunk = std::make_unique<FixedListColumnChunk>(dataType, copyDescription);
        break;
    case PhysicalTypeID::STRING:
        chunk = std::make_unique<StringColumnChunk>(dataType, copyDescription);
        break;
    case PhysicalTypeID::VAR_LIST:
        chunk = std::make_unique<VarListColumnChunk>(dataType, copyDescription);
        break;
    case PhysicalTypeID::STRUCT:
        chunk = std::make_unique<StructColumnChunk>(dataType, copyDescription);
        break;
    default: {
        throw NotImplementedException("ColumnChunkFactory::createColumnChunk for data type " +
                                      LogicalTypeUtils::dataTypeToString(dataType) +
                                      " is not supported.");
    }
    }
    chunk->initialize(StorageConstants::NODE_GROUP_SIZE);
    return chunk;
}

// Bool
template<>
void ColumnChunk::setValueFromString<bool>(const char* value, uint64_t length, uint64_t pos) {
    std::istringstream boolStream{std::string(value)};
    bool booleanVal;
    boolStream >> std::boolalpha >> booleanVal;
    setValue(booleanVal, pos);
}

// Fixed list
template<>
void ColumnChunk::setValueFromString<uint8_t*>(const char* value, uint64_t length, uint64_t pos) {
    auto fixedListVal =
        TableCopyUtils::getArrowFixedList(value, 1, length - 2, dataType, *copyDescription);
    memcpy(buffer.get() + pos * numBytesPerValue, fixedListVal.get(), numBytesPerValue);
}

// Interval
template<>
void ColumnChunk::setValueFromString<interval_t>(const char* value, uint64_t length, uint64_t pos) {
    auto val = Interval::fromCString(value, length);
    setValue(val, pos);
}

// Date
template<>
void ColumnChunk::setValueFromString<date_t>(const char* value, uint64_t length, uint64_t pos) {
    auto val = Date::fromCString(value, length);
    setValue(val, pos);
}

// Timestamp
template<>
void ColumnChunk::setValueFromString<timestamp_t>(
    const char* value, uint64_t length, uint64_t pos) {
    auto val = Timestamp::fromCString(value, length);
    setValue(val, pos);
}

offset_t ColumnChunk::getOffsetInBuffer(offset_t pos) const {
    auto numElementsInAPage =
        PageUtils::getNumElementsInAPage(numBytesPerValue, false /* hasNull */);
    auto posCursor = PageUtils::getPageByteCursorForPos(pos, numElementsInAPage, numBytesPerValue);
    auto offsetInBuffer =
        posCursor.pageIdx * BufferPoolConstants::PAGE_4KB_SIZE + posCursor.offsetInPage;
    assert(offsetInBuffer + numBytesPerValue <= numBytes);
    return offsetInBuffer;
}

void NullColumnChunk::resize(uint64_t numValues) {
    auto numBytesAfterResize = numBytesForValues(numValues);
    assert(numBytesAfterResize > numBytes);
    auto reservedBuffer = std::make_unique<uint8_t[]>(numBytesAfterResize);
    memset(reservedBuffer.get(), 0 /* non null */, numBytesAfterResize);
    memcpy(reservedBuffer.get(), buffer.get(), numBytes);
    buffer = std::move(reservedBuffer);
    numBytes = numBytesAfterResize;
}

void NullColumnChunk::setRangeNoNull(offset_t startPosInChunk, uint32_t numValuesToSet) {
    NullMask::setNullRange((uint64_t*)buffer.get(), startPosInChunk, numValuesToSet, false);
}

} // namespace storage
} // namespace kuzu
