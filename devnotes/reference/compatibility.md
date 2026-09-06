The play-test list — 29 games, one set each

Everything in §1, flattened: the games that boot and render, with the one set to load. **No clones,
no variants** — where the parent does not work the working set is named instead (`manxttc`, `hotdo`,
`vonj`). This is the list to sit down and play through.

| # | Game | Load |
|---:|---|---|
| 1 | Behind Enemy Lines | `bel` | - wtf is the input
| 2X | Daytona USA | `daytona` |
| 3X | Dead or Alive | `doa` | - kasumi broken
| 4 | Desert Tank | `desert` | - controls crazy but ok
| 5 | Dynamite Baseball 97 | `dynabb97` | - no idea
| 6 | Dynamite Cop | `dynamcop` | - ok
| 7 | Fighting Vipers | `fvipers` |-ok
| 8 | Gunblade NY | `gunblade` | - shootiis ng ok no cursor
| 9 | INDY 500 | `indy500` | Shifters dont work
| 10 | Last Bronx | `lastbrnx` | - ok flickers
| 11 | Manx TT Superbike | `manxttc` | good, use this
| 12 | Motor Raid | `motoraid` | good
| 13 | Over Rev | `overrev` |  ok
| 14 | Pilot Kids | `pltkids` | ok
| 15 | Rail Chase 2 | `rchase2` | cant shoot
| 16X | Sega Rally Championship | `srallyc` | ok
| 17 | Sega Touring Car Championship | `stcc` | ok
| 18 | Sega Water Ski | `segawski` |
| 19 | Sky Target | `skytargt` |
| 20 | Sonic Championship | `schamp` |
| 21 | Super GT 24h | `sgt24h` | shifter broken
| 22 | The House of the Dead | `hotdo` |
| 23 | Virtua Cop | `vcop` |
| 24 | Virtua Cop 2 | `vcop2` |
| 25 | Virtua Fighter 2 | `vf2` |
| 26 | Virtua Striker | `vstriker` |
| 27 | Virtual-On | `vonj` |
| 28 | Wave Runner | `waverunr` |
| 29 | Zero Gunner | `zerogun` |

⚠️ **"Working" here means it boots and draws.** Only 12 are A/B fixtures and only 8 are savestate
fixtures — the **Depth of verification** column in §1 says which, and the rest have had nothing but a
boot sweep. Playing them is exactly the gap this list exists to close.

Two need a non-pad device: **`vcop`** and **`vcop2`** want the **Light Gun** on the player's own port
(reload by shooting off screen). **Nine declare an `IPT_PADDLE`** and are therefore shaped by the
steering-curve options — `daytona`, `desert`, `indy500`, `manxttc`, `motoraid`, `overrev`, `srallyc`,
`sgt24h`, `stcc` (checked against the driver's five paddle-bearing port sets and what inherits them,
not inferred from the genre). ⚠️ **`waverunr` steers too and is not among them** — its handlebar is an
`IPT_AD_STICK_X`, so the curve does not touch it; that is the open question in
[steering-curve.md](steering-curve.md) §5.3.

---