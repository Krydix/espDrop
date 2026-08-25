# Licensing and contribution policy

espDrop is licensed under the GNU General Public License, version 3 or (at
your option) any later version. The SPDX identifier is
`GPL-3.0-or-later`. The complete license is in [LICENSE](../LICENSE).

## What this permits

The GPL permits private, public, academic, and commercial use. You may modify,
sell, and deploy espDrop. If you distribute covered binaries or firmware, you
must also make the complete corresponding source available under the GPL and
preserve recipients' rights to run, study, modify, and redistribute it. The
license text controls if this summary differs from it.

Running modified firmware on devices you operate, without conveying copies to
someone else, generally does not create a source-distribution requirement.
Distribution models and linked proprietary components can be fact-specific;
obtain legal advice when that boundary matters to a product.

## Version history

Versions through commit `352f924` were published under the MIT License. Those
existing MIT grants cannot be withdrawn. The relicensing commit and subsequent
versions are distributed as `GPL-3.0-or-later`.

## Contributions

Unless a contribution explicitly states otherwise, intentionally submitted
contributions are accepted under `GPL-3.0-or-later`. Contributors must have
the right to submit their work and must identify code derived from another
project.

## Upstream-derived code

OWL and OpenDrop source notices use GPL version 3 or any later version, which
is compatible with espDrop's license. When adapting their code, retain the
original copyright and license notice, name the upstream project and revision,
and describe significant changes.

The BLE wake wire profile was cross-checked against `airdrop-mt7921` revision
`d7c86192e3b79c520fde5965ddc24a1ad8cd1066`. espDrop independently encodes the
documented manufacturer record through ESP-IDF NimBLE; it does not carry the
project's BlueZ shell implementation.

esp-owl has mixed per-file notices and no repository-level license at the
revision currently recorded in the research ledger. Review each candidate
file independently. Do not copy code whose redistribution terms are absent or
unclear.
