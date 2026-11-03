// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/basic_types.h"

#include <QString>

namespace SexCat {

struct ScanSummary {
	int scanned = 0;
	int alreadyMarked = 0;
	int hashed = 0;
	int linked = 0;
	int imported = 0;
	int marked = 0;
	int failed = 0;
	bool finished = false;
};

// Backfills sha markers under a folder, relinking known content.
void StartFolderScan(
	const QString &root,
	Fn<void(const ScanSummary &)> progress);

} // namespace SexCat
