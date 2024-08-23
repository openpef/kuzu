#include "processor/operator/persistent/reader/csv/base_csv_reader.h"

#include <fcntl.h>

#include <vector>

#include "common/file_system/virtual_file_system.h"
#include "common/string_format.h"
#include "common/string_utils.h"
#include "common/system_message.h"
#include "main/client_context.h"
#include "processor/operator/persistent/reader/csv/driver.h"

using namespace kuzu::common;

namespace kuzu {
namespace processor {

BaseCSVReader::BaseCSVReader(const std::string& filePath, common::CSVOption option,
    CSVColumnInfo columnInfo, main::ClientContext* context, CSVFileErrorHandler* errorHandler)
    : context{context}, option{std::move(option)}, columnInfo{std::move(columnInfo)},
      currentBlockIdx(0), numRowsInCurrentBlock(0), curRowIdx(0), numErrors(0), buffer{nullptr},
      bufferIdx(0), bufferSize{0}, position{0}, lineContext(), osFileOffset{0},
      errorHandler(errorHandler), rowEmpty{false} {
    fileInfo = context->getVFSUnsafe()->openFile(filePath,
        O_RDONLY
#ifdef _WIN32
            | _O_BINARY
#endif
        ,
        context);
}

bool BaseCSVReader::isEOF() const {
    return getFileOffset() >= fileInfo->getFileSize();
}

uint64_t BaseCSVReader::getFileSize() {
    return fileInfo->getFileSize();
}

template<typename Driver>
bool BaseCSVReader::addValue(Driver& driver, uint64_t rowNum, column_id_t columnIdx,
    std::string_view strVal, std::vector<uint64_t>& escapePositions) {
    // insert the line number into the chunk
    if (!escapePositions.empty()) {
        // remove escape characters (if any)
        std::string newVal = "";
        uint64_t prevPos = 0;
        for (auto i = 0u; i < escapePositions.size(); i++) {
            auto nextPos = escapePositions[i];
            newVal += strVal.substr(prevPos, nextPos - prevPos);
            prevPos = nextPos + 1;
        }
        newVal += strVal.substr(prevPos, strVal.size() - prevPos);
        escapePositions.clear();
        return driver.addValue(rowNum, columnIdx, newVal);
    } else {
        return driver.addValue(rowNum, columnIdx, strVal);
    }
}

struct SkipRowDriver {
    explicit SkipRowDriver(uint64_t skipNum) : skipNum{skipNum} {}
    bool done(uint64_t rowNum) const { return rowNum >= skipNum; }
    bool addRow(uint64_t, column_id_t) { return true; }
    bool addValue(uint64_t, column_id_t, std::string_view) { return true; }

    uint64_t skipNum;
};

uint64_t BaseCSVReader::handleFirstBlock() {
    uint64_t numRowsRead = 0;
    readBOM();
    if (option.skipNum > 0) {
        SkipRowDriver driver{option.skipNum};
        numRowsRead += parseCSV(driver);
    }
    if (option.hasHeader) {
        numRowsRead += readHeader();
    }
    return numRowsRead;
}

void BaseCSVReader::readBOM() {
    if (!maybeReadBuffer(nullptr)) {
        return;
    }
    if (bufferSize >= 3 && buffer[0] == '\xEF' && buffer[1] == '\xBB' && buffer[2] == '\xBF') {
        position = 3;
    }
}

// Dummy driver that just skips a row.
struct HeaderDriver {
    bool done(uint64_t) { return true; }
    bool addRow(uint64_t, column_id_t) { return true; }
    bool addValue(uint64_t, column_id_t, std::string_view) { return true; }
};

void BaseCSVReader::resetNumRowsInCurrentBlock() {
    numRowsInCurrentBlock = 0;
}

void BaseCSVReader::increaseNumRowsInCurrentBlock(uint64_t numRows) {
    numRowsInCurrentBlock += numRows;
}

uint64_t BaseCSVReader::getNumRowsInCurrentBlock() const {
    return numRowsInCurrentBlock;
}

uint64_t BaseCSVReader::readHeader() {
    HeaderDriver driver;
    return parseCSV(driver);
}

bool BaseCSVReader::readBuffer(uint64_t* start) {
    std::unique_ptr<char[]> oldBuffer = std::move(buffer);

    // the remaining part of the last buffer
    uint64_t remaining = 0;
    if (start != nullptr) {
        KU_ASSERT(*start <= bufferSize);
        remaining = bufferSize - *start;
    }

    uint64_t bufferReadSize = CopyConstants::INITIAL_BUFFER_SIZE;
    while (remaining > bufferReadSize) {
        bufferReadSize *= 2;
    }

    buffer = std::unique_ptr<char[]>(new char[bufferReadSize + remaining + 1]());
    if (remaining > 0) {
        // remaining from last buffer: copy it here
        KU_ASSERT(start != nullptr);
        memcpy(buffer.get(), oldBuffer.get() + *start, remaining);
    }
    auto readCount = fileInfo->readFile(buffer.get() + remaining, bufferReadSize);
    if (readCount == -1) {
        // LCOV_EXCL_START
        lineContext.setEndOfLine(getFileOffset());
        handleCopyException(stringFormat("Could not read from file: {}", posixErrMessage()), true);
        // LCOV_EXCL_STOP
    }

    // Update buffer size in a way so that the invariant osFileOffset >= bufferSize is never broken
    // This is needed because in the serial CSV reader the progressFunc can call getFileOffset from
    // a different thread
    bufferSize = remaining;
    osFileOffset += readCount;
    bufferSize += readCount;

    buffer[bufferSize] = '\0';
    if (start != nullptr) {
        *start = 0;
    }
    position = remaining;
    ++bufferIdx;
    return readCount > 0;
}

std::string BaseCSVReader::reconstructLine(uint64_t startPosition, uint64_t endPosition) {
    KU_ASSERT(endPosition >= startPosition);

    if (-1 == fileInfo->seek(startPosition, SEEK_SET)) {
        return "Unable to reconstruct line";
    }

    std::string res;
    res.resize(endPosition - startPosition);

    (void)fileInfo->readFile(res.data(), res.size());

    [[maybe_unused]] const auto reseekSuccess = fileInfo->seek(osFileOffset, SEEK_SET);
    KU_ASSERT(-1 != reseekSuccess);

    return StringUtils::ltrimNewlines(StringUtils::rtrimNewlines(res));
}

void BaseCSVReader::skipCurrentLine() {
    do {
        for (; position < bufferSize; ++position) {
            if (isNewLine(buffer[position])) {
                while (position < bufferSize && isNewLine(buffer[position])) {
                    ++position;
                }
                return;
            }
        }
    } while (maybeReadBuffer(nullptr));
}

void BaseCSVReader::handleCopyException(const std::string& message, bool mustThrow) {
    CSVError error{.message = message,
        .errorLine = lineContext,
        .blockIdx = currentBlockIdx,
        .numRowsReadInBlock = numRowsInCurrentBlock + curRowIdx + numErrors,
        .mustThrow = mustThrow};
    if (!error.errorLine.isCompleteLine) {
        error.errorLine.endByteOffset = getFileOffset();
    }
    errorHandler->handleError(this, error);

    // if we reach here it means we are ignoring the error
    ++numErrors;
}

template<typename Driver>
uint64_t BaseCSVReader::parseCSV(Driver& driver) {
    KU_ASSERT(nullptr != errorHandler);

    // used for parsing algorithm
    curRowIdx = 0;
    numErrors = 0;

    while (true) {
        column_id_t column = 0;
        auto start = position;
        bool hasQuotes = false;
        std::vector<uint64_t> escapePositions;
        lineContext.setNewLine(getFileOffset());

        // read values into the buffer (if any)
        if (!maybeReadBuffer(&start)) {
            return curRowIdx;
        }

        // start parsing the first value
        goto value_start;
    value_start:
        // state: value_start
        // this state parses the first character of a value
        if (buffer[position] == option.quoteChar) {
            [[unlikely]]
            // quote: actual value starts in the next position
            // move to in_quotes state
            start = position + 1;
            hasQuotes = true;
            goto in_quotes;
        } else {
            // no quote, move to normal parsing state
            start = position;
            hasQuotes = false;
            goto normal;
        }
    normal:
        // state: normal parsing state
        // this state parses the remainder of a non-quoted value until we reach a delimiter or
        // newline
        do {
            for (; position < bufferSize; position++) {
                if (buffer[position] == option.delimiter) {
                    // delimiter: end the value and add it to the chunk
                    goto add_value;
                } else if (isNewLine(buffer[position])) {
                    // newline: add row
                    goto add_row;
                }
            }
        } while (readBuffer(&start));

        [[unlikely]]
        // file ends during normal scan: go to end state
        goto final_state;
    add_value:
        // We get here after we have a delimiter.
        KU_ASSERT(buffer[position] == option.delimiter);
        // Trim one character if we have quotes.
        if (!addValue(driver, curRowIdx, column,
                std::string_view(buffer.get() + start, position - start - hasQuotes),
                escapePositions)) {
            goto ignore_error;
        }
        column++;

        // Move past the delimiter.
        ++position;
        // Adjust start for MaybeReadBuffer.
        start = position;
        if (!maybeReadBuffer(&start)) {
            [[unlikely]]
            // File ends right after delimiter, go to final state
            goto final_state;
        }
        goto value_start;
    add_row: {
        // We get here after we have a newline.
        KU_ASSERT(isNewLine(buffer[position]));
        lineContext.setEndOfLine(getFileOffset());
        bool isCarriageReturn = buffer[position] == '\r';
        if (!addValue(driver, curRowIdx, column,
                std::string_view(buffer.get() + start, position - start - hasQuotes),
                escapePositions)) {
            goto ignore_error;
        }
        column++;

        curRowIdx += driver.addRow(curRowIdx, column);

        column = 0;
        position++;
        // Adjust start for ReadBuffer.
        start = position;
        lineContext.setNewLine(getFileOffset());
        if (!maybeReadBuffer(&start)) {
            // File ends right after newline, go to final state.
            goto final_state;
        }
        if (isCarriageReturn) {
            // \r newline, go to special state that parses an optional \n afterwards
            goto carriage_return;
        } else {
            if (driver.done(curRowIdx)) {
                return curRowIdx;
            }
            goto value_start;
        }
    }
    in_quotes:
        // this state parses the remainder of a quoted value.
        position++;
        do {
            for (; position < bufferSize; position++) {
                if (buffer[position] == option.quoteChar) {
                    // quote: move to unquoted state
                    goto unquote;
                } else if (buffer[position] == option.escapeChar) {
                    // escape: store the escaped position and move to handle_escape state
                    escapePositions.push_back(position - start);
                    goto handle_escape;
                } else if (isNewLine(buffer[position])) {
                    [[unlikely]] if (!handleQuotedNewline()) { goto ignore_error; }
                }
            }
        } while (readBuffer(&start));
        [[unlikely]]
        // still in quoted state at the end of the file, error:
        lineContext.setEndOfLine(getFileOffset());
        handleCopyException("unterminated quotes.");
        // we are ignoring this error, skip current row and restart state machine
        goto ignore_error;
    unquote:
        KU_ASSERT(hasQuotes && buffer[position] == option.quoteChar);
        // this state handles the state directly after we unquote
        // in this state we expect either another quote (entering the quoted state again, and
        // escaping the quote) or a delimiter/newline, ending the current value and moving on to the
        // next value
        position++;
        if (!maybeReadBuffer(&start)) {
            // file ends right after unquote, go to final state
            goto final_state;
        }
        if (buffer[position] == option.quoteChar &&
            (!option.escapeChar || option.escapeChar == option.quoteChar)) {
            // escaped quote, return to quoted state and store escape position
            escapePositions.push_back(position - start);
            goto in_quotes;
        } else if (buffer[position] == option.delimiter ||
                   buffer[position] == CopyConstants::DEFAULT_CSV_LIST_END_CHAR) {
            // delimiter, add value
            goto add_value;
        } else if (isNewLine(buffer[position])) {
            goto add_row;
        } else {
            [[unlikely]] handleCopyException("quote should be followed by "
                                             "end of file, end of value, end of "
                                             "row or another quote.");
            goto ignore_error;
        }
    handle_escape:
        // state: handle_escape
        // escape should be followed by a quote or another escape character
        position++;
        if (!maybeReadBuffer(&start)) {
            [[unlikely]] lineContext.setEndOfLine(getFileOffset());
            handleCopyException("escape at end of file.");
            goto ignore_error;
        }
        if (buffer[position] != option.quoteChar && buffer[position] != option.escapeChar) {
            ++position; // consume the invalid char
            [[unlikely]] handleCopyException("neither QUOTE nor ESCAPE is proceeded by ESCAPE.");
            goto ignore_error;
        }
        // escape was followed by quote or escape, go back to quoted state
        goto in_quotes;
    carriage_return:
        // this stage optionally skips a newline (\n) character, which allows \r\n to be interpreted
        // as a single line

        // position points to the character after the carriage return.
        if (buffer[position] == '\n') {
            // newline after carriage return: skip
            // increase position by 1 and move start to the new position
            start = ++position;
            if (!maybeReadBuffer(&start)) {
                // file ends right after newline, go to final state
                goto final_state;
            }
        }
        if (driver.done(curRowIdx)) {
            return curRowIdx;
        }

        goto value_start;
    final_state:
        // We get here when the file ends.
        // If we were mid-value, add the remaining value to the chunk.
        lineContext.setEndOfLine(getFileOffset());
        if (position > start) {
            // Add remaining value to chunk.
            if (!addValue(driver, curRowIdx, column,
                    std::string_view(buffer.get() + start, position - start - hasQuotes),
                    escapePositions)) {
                return curRowIdx;
            }
            column++;
        }
        if (column > 0) {
            curRowIdx += driver.addRow(curRowIdx, column);
        }
        return curRowIdx;
    ignore_error:
        // we skip the current row then restart the state machine to continue parsing
        skipCurrentLine();
        continue;
    }
    KU_UNREACHABLE;
}

template uint64_t BaseCSVReader::parseCSV<ParallelParsingDriver>(ParallelParsingDriver&);
template uint64_t BaseCSVReader::parseCSV<SerialParsingDriver>(SerialParsingDriver&);
template uint64_t BaseCSVReader::parseCSV<SniffCSVNameAndTypeDriver>(SniffCSVNameAndTypeDriver&);

uint64_t BaseCSVReader::getFileOffset() const {
    KU_ASSERT(osFileOffset >= bufferSize);
    return osFileOffset - bufferSize + position;
}

} // namespace processor
} // namespace kuzu
