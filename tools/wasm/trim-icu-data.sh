#!/usr/bin/env bash
# trim-icu-data.sh <icupkg> <in.dat> <out.dat> <workdir>
#
# Shrinks ICU's data archive (icudtNNl.dat, 30 MB for 74.2) to what
# FreeCAD's core needs (~2.6 MB): root + English locale data in every tree
# (main, curr, lang, region, unit, zone), the locale-independent tables
# (properties, normalization, numbering systems, plurals, ...).  Called by
# build-deps.sh (stage icudata).  Anything not English falls back to root.
#
# Found the hard way: curr/ must stay - DecimalFormatSymbols reads the
# currency data and fails with U_MISSING_RESOURCE_ERROR without it.
set -euo pipefail
icupkg=$1 in=$2 out=$3 work=$4
"$icupkg" -l "$in" > "$work/icu-items.txt"
{
    # collation, break-iterator rules/dictionaries, transliteration, rule-based
    # number spelling: FreeCAD uses none of them
    grep -E '^(coll|brkitr|translit|rbnf)/' "$work/icu-items.txt" || true
    # table-based converters (ibm-*, gb18030, ...); UTF-8/16/32, Latin-1 and
    # ASCII are algorithmic in ICU and need no .cnv
    grep -E '\.cnv$' "$work/icu-items.txt" || true
    # per-locale bundles in every tree (main, curr, lang, region, unit, zone)
    # except root and English
    grep -E '^([a-z]+/)?[a-z]{2,3}(_[A-Za-z0-9]+)*\.res$' "$work/icu-items.txt" \
        | grep -vE '^([a-z]+/)?(root|en|en_US|en_US_POSIX|en_001|pool|res_index|metadata|plurals|units|zoneinfo64|timezoneTypes|windowsZones|metaZones|keyTypeData|dayPeriods|genderList|icuver|icustd|tzdbNames|numberingSystems|supplementalData|likelySubtags|pluralRanges|grammaticalFeatures|characterProperties|currencyNumericCodes|langInfo)\.res$' || true
} | sort -u > "$work/icu-remove.txt"
cp "$in" "$out"
"$icupkg" -r "$work/icu-remove.txt" "$out" > /dev/null
