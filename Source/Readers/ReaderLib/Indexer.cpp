//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#define __STDC_FORMAT_MACROS
#define _CRT_SECURE_NO_WARNINGS
#include <inttypes.h>
#include "Indexer.h"

using std::string;

const static char ROW_DELIMITER = '\n';

namespace Microsoft { namespace MSR { namespace CNTK {

Indexer::Indexer(bool isPrimary, FILE* file, bool skipSequenceIds, bool numericSequenceId, char streamPrefix, size_t chunkSize, size_t bufferSize) :
    m_streamPrefix(streamPrefix),
    m_numericSequenceId(numericSequenceId),
    m_bufferSize(bufferSize),
    m_file(file),
    m_fileOffsetStart(0),
    m_fileOffsetEnd(0),
    m_buffer(new char[bufferSize + 1]),
    m_bufferStart(nullptr),
    m_bufferEnd(nullptr),
    m_pos(nullptr),
    m_done(false),
    m_hasSequenceIds(!skipSequenceIds),
    m_index(chunkSize, isPrimary)
{
    if (m_file == nullptr)
    {
        RuntimeError("Input file not open for reading");
    }
}

void Indexer::RefillBuffer()
{
    if (!m_done)
    {
        size_t bytesRead = fread(m_buffer.get(), 1, m_bufferSize, m_file);
        if (bytesRead == (size_t)-1)
            RuntimeError("Could not read from the input file.");
        if (bytesRead == 0)
        {
            m_done = true;
        }
        else
        {
            m_fileOffsetStart = m_fileOffsetEnd;
            m_fileOffsetEnd += bytesRead;
            m_bufferStart = m_buffer.get();
            m_pos = m_bufferStart;
            m_bufferEnd = m_bufferStart + bytesRead;
        }
    }
}

void Indexer::BuildFromLines(CorpusDescriptorPtr corpus)
{
    assert(m_pos == m_bufferStart);
    m_hasSequenceIds = false;
    size_t lines = 0;
    int64_t offset = GetFileOffset();
    while (!m_done)
    {
        m_pos = (char*)memchr(m_pos, ROW_DELIMITER, m_bufferEnd - m_pos);
        if (m_pos)
        {
            SequenceDescriptor sd = {};
            sd.m_numberOfSamples = 1;
            sd.m_fileOffsetBytes = offset;
            offset = GetFileOffset() + 1;
            sd.m_byteSize = offset - sd.m_fileOffsetBytes;
            AddSequenceIfIncluded(corpus, lines, sd);
            ++m_pos;
            ++lines;
        }
        else
        {
            RefillBuffer();
        }
    }

    if (offset < m_fileOffsetEnd)
    {
        // There's a number of characters, not terminated by a newline,
        // add a sequence to the index, parser will have to deal with it.
        SequenceDescriptor sd = {};
        sd.m_numberOfSamples = 1;
        sd.m_fileOffsetBytes = offset;
        sd.m_byteSize = m_fileOffsetEnd - sd.m_fileOffsetBytes;
        AddSequenceIfIncluded(corpus, lines, sd);
    }
}

void Indexer::Build(CorpusDescriptorPtr corpus)
{
    if (!m_index.IsEmpty())
    {
        return;
    }

    m_index.Reserve(filesize(m_file));

    RefillBuffer(); // read the first block of data
    if (m_done)
    {
        RuntimeError("Input file is empty");
    }

    if ((m_bufferEnd - m_bufferStart > 3) &&
        (m_bufferStart[0] == '\xEF' && m_bufferStart[1] == '\xBB' && m_bufferStart[2] == '\xBF'))
    {
        // input file contains UTF-8 BOM value, skip it.
        m_pos += 3;
        m_fileOffsetStart += 3;
        m_bufferStart += 3;
    }

    // check the first byte and decide what to do next
    if (!m_hasSequenceIds || m_bufferStart[0] == m_streamPrefix)
    {
        // skip sequence id parsing, treat lines as individual sequences
        BuildFromLines(corpus);
        return;
    }

    size_t id = 0;
    int64_t offset = GetFileOffset();
    // read the very first sequence id
    if (!TryGetSequenceId(id, corpus->GetStringRegistry()))
    {
        RuntimeError("Expected a sequence id at the offset %" PRIi64 ", none was found.", offset);
    }

    SequenceDescriptor sd = {};
    sd.m_fileOffsetBytes = offset;

    size_t currentKey = id;
    while (!m_done)
    {
        SkipLine(); // ignore whatever is left on this line.
        offset = GetFileOffset(); // a new line starts at this offset;
        sd.m_numberOfSamples++;

        if (!m_done && TryGetSequenceId(id, corpus->GetStringRegistry()) && id != currentKey)
        {
            // found a new sequence, which starts at the [offset] bytes into the file
            sd.m_byteSize = offset - sd.m_fileOffsetBytes;
            AddSequenceIfIncluded(corpus, currentKey, sd);

            sd = {};
            sd.m_fileOffsetBytes = offset;
            currentKey = id;
        }
    }

    // calculate the byte size for the last sequence
    sd.m_byteSize = m_fileOffsetEnd - sd.m_fileOffsetBytes;
    AddSequenceIfIncluded(corpus, currentKey, sd);
}

void Indexer::AddSequenceIfIncluded(CorpusDescriptorPtr corpus, size_t sequenceId, SequenceDescriptor& sd)
{
    if (m_numericSequenceId)
    {
        auto& stringRegistry = corpus->GetStringRegistry();
        auto key = std::to_string(sequenceKey);
        if (corpus->IsIncluded(key))
        {
            sd.m_key.m_sequence = stringRegistry[key];
            sd.m_key.m_sample = 0;
            m_index.AddSequence(sd);
        }
    }
    else
    {
        auto& stringRegistry = corpus->GetStringRegistry();
        auto key = corpus->GetStringRegistry()[sequenceKey];
        if (corpus->IsIncluded(key))
        {
            sd.m_key.m_sequence = stringRegistry[key];
            sd.m_key.m_sample = 0;
            m_index.AddSequence(sd);
        }
    }
}

void Indexer::SkipLine()
{
    while (!m_done)
    {
        m_pos = (char*)memchr(m_pos, ROW_DELIMITER, m_bufferEnd - m_pos);
        if (m_pos)
        {
            //found a new-line character
            if (++m_pos == m_bufferEnd)
            {
                RefillBuffer();
            }
            return;
        }
        RefillBuffer();
    }
}

bool Indexer::TryGetSequenceId(size_t& id, StringToIdMap& mapping)
{
    bool found = false;
    id = 0;
    std::string key;
    while (!m_done)
    {
        while (m_pos != m_bufferEnd)
        {
            char c = *m_pos;

            if (m_numericSequenceId)
            {
                if (!isdigit(c))
                {
                    // Stop as soon as there's a non-digit character
                    return found;
                }
                id = id * 10 + (c - '0');
            }
            else
            {
                if (isspace(c))
                {
                    if (found && !mapping.TryGet(key, id))
                    {
                        id = mapping.AddValue(key);
                    }
                    // Stop as soon as there's a non-digit character
                    return found;
                }
                key += c;
            }

            found |= true;
            ++m_pos;
        }
        RefillBuffer();
    }

    // reached EOF without hitting the pipe character,
    // ignore it for not, parser will have to deal with it.
    return false;
}

}}}
