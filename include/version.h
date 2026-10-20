// The one place the version number lives.
//
// Both .rc files stamp their VERSIONINFO from it, and the release
// workflow reads KRYOTOO_VERSION_STR out of this file with a regex
// to decide whether a tag is owed. That is the whole release
// trigger: bump the number here, write the CHANGELOG entry, push.
// No tagging by hand.
//
// Keep the string and the three numbers in agreement - nothing
// derives one from the other, because the resource compiler needs
// them as bare integers and the workflow needs them as text.
#pragma once

#define KRYOTOO_VERSION_MAJOR 1
#define KRYOTOO_VERSION_MINOR 9
#define KRYOTOO_VERSION_PATCH 1

#define KRYOTOO_VERSION_STR "1.9.1"
