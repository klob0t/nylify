#pragma once

class MFRC522;

// Wiring self-check for the DIAG serial command: probes both modules
// independently so a fault can be pinned to one bus rather than guessed at.
namespace diag {

void run(MFRC522& rfid);

}  // namespace diag
