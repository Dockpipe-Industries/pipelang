// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <initializer_list>

namespace {
struct ProtocolStreams {
    ProtocolStreams() {
        // Compiler protocols are UTF-8 bytes. Text-mode CRLF and Ctrl-Z
        // translation would change exact outputs and truncate valid requests.
        for (FILE* stream : {stdin, stdout, stderr}) {
            const int descriptor = _fileno(stream);
            if (descriptor >= 0 && _setmode(descriptor, _O_BINARY) == -1)
                std::abort();
        }
    }
};
const ProtocolStreams protocolStreams;
}
