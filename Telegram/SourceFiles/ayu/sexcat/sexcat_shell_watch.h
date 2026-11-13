// This is the source code of [R:67494147] for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ayu/sexcat/sexcat_database.h"

#include <QString>

#include <optional>

namespace SexCat {

// Relinks saved files after shell renames and same-content moves.
void StartShellWatch();

// Feeds every recent row whose file went missing into the bounded
// relocate search; used by the manual history refresh.
void RelocateMissingEntries();

// Bounded same-name-and-size search under one root, for manual relocates.
[[nodiscard]] std::optional<QString> LocateByNameSize(
	const QString &root,
	const QString &fileName,
	int64 fileSize);

// Bounded search for every missing record under one picked root;
// relinks hits in place and returns how many rows moved.
[[nodiscard]] int RelocateLostInFolder(const QString &root);

void RelinkMovedEntry(const Entry &entry, const QString &newPath);

} // namespace SexCat
