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

void TryAutoRelocate(const Entry &entry);

// Bounded same-name-and-size search under one root, for manual relocates.
[[nodiscard]] std::optional<QString> LocateByNameSize(
	const QString &root,
	const QString &fileName,
	int64 fileSize);

void RelinkMovedEntry(const Entry &entry, const QString &newPath);

} // namespace SexCat
