# Donationware pivot (2026-07) — Mac side TODO

The project moved from MIT to **donationware** (owner decision, before any
downloads happened): free to download/use/share, source on GitHub, donations at
https://buymeacoffee.com/lorasandlenses appreciated but never required. The
`LICENSE` file at the repo root holds the actual terms; the README hero and
Support sections carry the positioning ("Free donationware · source on GitHub",
"no locked features, no nags... buy me a coffee. That's the whole business
model."). Don't call the project "open source (MIT)" anywhere anymore.

**Done on Windows** (`source/irate.cpp`, paintHelp): the H shortcuts overlay
gained one extra row at the bottom, centered, in the gold accent colour
(RGB 255,200,40):

> iRate is donationware — if it saves your evenings: buymeacoffee.com/lorasandlenses

**Mac shell TODO — mirror this:** add the same single line at the bottom of the
H help overlay in `mac/main.mm`, same wording, gold accent, below the existing
reject-behaviour footnote. Nothing else in-app changes — no nag dialogs, no
startup screens; the Help footer and the website/README are the only places the
ask appears.
