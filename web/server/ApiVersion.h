#pragma once

/*
 * The version of the JSON contract between this server and the browser bundle
 * it serves. Every /api/ response carries it, and a page running a bundle built
 * against a different number reloads itself to fetch the matching one.
 *
 * Bump it only when an already-loaded bundle would misread a new response: a
 * field renamed or removed, a unit or a meaning changed, a different number of
 * rating buckets. A new field, a new endpoint, a different .ote behind the same
 * shape — none of those are visible to an old bundle, and bumping for them
 * reloads every open page for nothing.
 *
 * The UI build reads this literal out of this file, so the two halves of the
 * image cannot disagree about which contract they were built for. That parse
 * wants a plain decimal on one line.
 */
constexpr int API_VERSION = 1;
