Đây là layout (expected) của dự án, có thể thay đổi tùy vào tình hìn thực tế
```
SocketProgramming/
├── server/
│   ├── src/
│   │   ├── main.cpp                        [Member A]
│   │   ├── control/                        [Member A]
│   │   │   ├── Session.h / .cpp
│   │   │   ├── CommandParser.h / .cpp
│   │   │   └── CommandDispatcher.h / .cpp
│   │   ├── rdt/                            [Member B]
│   │   │   ├── RdtHeader.h / .cpp
│   │   │   ├── RdtSender.h / .cpp
│   │   │   ├── RdtReceiver.h / .cpp
│   │   │   └── Checksum.h / .cpp
│   │   ├── filesystem/                     [Member C]
│   │   │   ├── PathResolver.h / .cpp
│   │   │   ├── DirectoryService.h / .cpp
│   │   │   ├── ChunkedFileReader.h / .cpp
│   │   │   └── ChunkedFileWriter.h / .cpp
│   │   ├── crypto/                         [Member C]
│   │   │   └── Sha256Hasher.h / .cpp
│   │   ├── datachannel/                    [Member C, interface co-designed with B]
│   │   │   ├── PassiveModeHandler.h / .cpp
│   │   │   ├── ActiveModeHandler.h / .cpp
│   │   │   └── DataChannelSession.h / .cpp
│   │   └── common/                         [shared, whoever needs it first writes it]
│   │       ├── ReplyCodes.h
│   │       └── Logger.h / .cpp
│   └── server_root/                        ← runtime storage sandbox, NOT source code, add to .gitignore contents
client/
├── src/
│   ├── main.cpp                            [Member A]
│   ├── cli/                                [Member A]
│   │   └── CommandLineInterface.h / .cpp
│   ├── control/                            [Member A]
│   │   └── ControlConnection.h / .cpp
│   ├── rdt/                                [Member B — duplicated from server/src/rdt/, same logic]
│   │   ├── RdtHeader.h / .cpp
│   │   ├── RdtSender.h / .cpp
│   │   ├── RdtReceiver.h / .cpp
│   │   └── Checksum.h / .cpp
│   ├── datachannel/                        [Member C + B — client-side mirror, different socket roles]
│   │   ├── ActiveModeClient.h / .cpp
│   │   ├── PassiveModeClient.h / .cpp
│   │   └── DataChannelSession.h / .cpp
│   ├── fileio/                             [Member C — duplicated from server/src/filesystem/]
│   │   ├── ChunkedFileReader.h / .cpp
│   │   └── ChunkedFileWriter.h / .cpp
│   └── crypto/                             [Member C — duplicated from server/src/crypto/]
│       └── Sha256Hasher.h / .cpp
├── tests/                                  [C own most of these]
│   ├── test_chunking.cpp
│   ├── test_path_safety.cpp
│   ├── test_hashing.cpp
│   └── test_data_channel.cpp
├── docs/
│   ├── report/                             (technical report .docx)
│   └── genai_log/                          (per-person GenAI log scratch files)
├── .gitignore
├── CMakeLists.txt
└── README.md
```
