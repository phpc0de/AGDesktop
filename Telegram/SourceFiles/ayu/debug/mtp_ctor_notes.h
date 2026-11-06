/*
This file is part of AyuGram for Telegram Desktop,
an unofficial fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/AyuGram/AyuGramDesktop/blob/dev/LEGAL
*/
#pragma once

namespace Ayu::Debug {

struct CtorNote {
	const char *name;
	const char *purpose;
};

struct FieldNote {
	const char *name;
	const char *note;
};

[[nodiscard]] const CtorNote *FindCtorNote(const char *name);
[[nodiscard]] const FieldNote *FindFieldNote(const char *name);

} // namespace Ayu::Debug
