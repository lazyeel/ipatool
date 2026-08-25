// Copyright 2026 lazyeel (https://github.com/lazyeel)
// SPDX-License-Identifier: Apache-2.0

#pragma once
// adi_anisette.h: native ADI anisette provider declaration.
//
// AnisetteData::generate_locally() is declared in anisette.h; on
// non-Windows builds this translation unit provides it, backed by the
// classic Apple Music 2.9.0 ADI stack loaded from ./libs-classic (or a
// discovered kit directory). See adi_anisette.cpp for the invariants.
