# st (crod build)

Personal build of [suckless st](https://st.suckless.org) with the Kitty graphics protocol and a stack of community patches.

See [`UPSTREAM.md`](UPSTREAM.md) for the original st-graphics README.

## Acknowledgements

- **[Sergei Grechanik](https://github.com/sergei-grechanik)** — author of [st-graphics](https://github.com/sergei-grechanik/st-graphics), the fork that adds the Kitty graphics protocol to st (Unicode placeholders, classic placement, image cache). This build is based on his `graphics-with-patches` branch.
- **[suckless](https://suckless.org)** — original [st](https://st.suckless.org) and the [patch ecosystem](https://st.suckless.org/patches/).
- Individual patch authors credited at their respective patch pages on suckless.org.

## Lineage

1. **Base:** [suckless st 0.9.3](https://git.suckless.org/st/) — the simple terminal.
2. **Graphics:** [sergei-grechanik/st-graphics](https://github.com/sergei-grechanik/st-graphics) by Sergei Grechanik. Tracked as the `upstream` remote.
3. **Patches:** community patches from [suckless.org/patches](https://st.suckless.org/patches/) on top.
4. **Local tweaks:** see [Local changes](#local-changes).

## Patches applied

Patch sources kept in [`patches/`](patches/) for provenance.

| Patch | Upstream link |
|---|---|
| anysize | https://st.suckless.org/patches/anysize/ |
| boxdraw_v2 | https://st.suckless.org/patches/boxdraw/ |
| clipboard | https://st.suckless.org/patches/clipboard/ |
| csi_22_23 | https://st.suckless.org/patches/csi_22_23/ |
| defaultfontsize | https://st.suckless.org/patches/defaultfontsize/ |
| externalpipe | https://st.suckless.org/patches/externalpipe/ |
| font2 | https://st.suckless.org/patches/font2/ |
| glyph-wide-support (boxdraw variant) | https://st.suckless.org/patches/glyph_wide_support/ |
| ligatures (boxdraw variant) | https://st.suckless.org/patches/ligatures/ |
| newterm | https://st.suckless.org/patches/newterm/ |
| scrollback | https://st.suckless.org/patches/scrollback/ |
| scrollback-mouse | https://st.suckless.org/patches/scrollback/ |
| scrollback-mouse-altscreen | https://st.suckless.org/patches/scrollback/ |
| application-sync (Synchronized-Updates) | https://st.suckless.org/patches/synchronized-updates/ |
| undercurl | https://st.suckless.org/patches/undercurl/ |
| vertcenter | https://st.suckless.org/patches/vertcenter/ |

## Local changes

On top of the patched tree:

- **No auto-snap to bottom on output.** `twrite` no longer resets `TSCREEN.off = 0`. Scroll up, select text, switch windows — terminal stays put when new output arrives. Use `Shift+PgDn` or scroll wheel down to return to the bottom.
- **Shift+Enter sends CSI-u (`\033[13;2u`).** Restores extended-key reporting needed by Claude Code (and other apps using the kitty keyboard protocol) so Shift+Enter inserts a newline instead of submitting.

## Build

```sh
make
sudo make install
```

Dependencies: X11, Xft, fontconfig, freetype2, harfbuzz, imlib2, zlib.

## Terminfo

```sh
tic -sx st.info
```

For tmux passthrough of graphics: `set -g allow-passthrough on` in `tmux.conf` (tmux ≥ 3.4).

## License

MIT/X (suckless). See [`LICENSE`](LICENSE).
