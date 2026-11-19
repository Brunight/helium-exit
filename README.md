# helium-exit

Puts [Helium](https://helium.computer)'s **Exit** item back in the top-level
three-dot menu (right after **Settings**) on **Linux**, instead of hiding it
under **More tools**.

Linux port of the idea behind
[hang333/fuck-helium](https://github.com/hang333/fuck-helium), which does the
same on Windows via a `version.dll` proxy.

## How it works

Helium builds the app menu in C++, so there's no setting or extension for this.
`helium-exit` is a tiny `LD_PRELOAD` library that, when the Helium browser
process starts:

1. Scans the executable's code in memory for
   `mov esi, 34031 (IDC_EXIT); mov edx, <string>; call AddItem` in the
   More tools menu, and `mov esi, 40015 (IDC_OPTIONS); …; call AddItem` in the
   app menu, both calling the same helper.
2. Only patches if **exactly one** validated pair is found (plus the separator
   right before Exit). Otherwise it leaves Helium untouched — fail-closed.
3. Redirects those two calls (via a small trampoline page): the Exit call is
   swallowed (its label and icon are remembered), and Exit is re-added right
   after Settings. The now-dangling separator in More tools is NOP'd.

Nothing on disk is modified, so Helium updates are unaffected. No addresses are
hard-coded. It's inert in any process other than the `helium` browser process
(wrapper scripts, renderers, GPU, zygote, crashpad), and removes itself from
`LD_PRELOAD` so child processes never load it.

## Requirements

- Linux, x86-64
- `gcc`
- Tested with Helium `0.21.3.1` (Chromium `157.0.8103.78`), Arch
  `helium-browser-bin` package

## Install

```sh
git clone https://github.com/Brunight/helium-exit
cd helium-exit
sh install.sh
```

This builds `~/.local/lib/helium-exit/libhelium_exit.so` and installs a
`~/.local/bin/helium-browser` wrapper that sets `LD_PRELOAD` and execs
`/usr/bin/helium-browser`. Anything that launches `helium-browser` via `PATH`
(including `.desktop` launchers, if `~/.local/bin` is in your session `PATH`)
picks it up. Fully quit and restart Helium afterwards.

If your Helium is installed elsewhere, edit the `exec` line in the wrapper — or
just launch with:

```sh
LD_PRELOAD=/path/to/libhelium_exit.so /path/to/helium
```

## Troubleshooting

```sh
HELIUM_EXIT_DEBUG=1 helium-browser --version
```

prints either `patched: …` or why the patch was skipped (e.g. a Helium update
changed the code shape).

## Uninstall

```sh
rm ~/.local/bin/helium-browser
rm -r ~/.local/lib/helium-exit
```

## License

MIT
