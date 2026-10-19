# Wii Dragon Net Battle menu in the PC port

With the Wii assets extracted, the PS2 main-menu plate **Dragon Net Battle** opens the Wii mode's top-level choices
in the port's screen mode. Without those assets, it opens the PC online window over the main menu as before.
Nintendo WFC Battle opens the PC online window over the Wii menu, like the F1 settings window.
The separate **Tenkaichi Budokai Online** plate still opens its in-game hub.
Manage Friends and User Settings remain selectable for their Wii artwork and
spoken descriptions; pressing Enter on either keeps the Wii menu open. The Wii home screen omits the port's bottom
right Enter and Back hints.

Extract the menu from a Wii disc you own:

```sh
python3 port/tools/extract_wii_online.py roms/RDSPAF.rvz
```

You can also include it in the normal install, with either a European Wii `.iso` or `.rvz` compressed image:

```sh
python3 install.py --iso /path/to/usa.iso --eu-iso roms/SLES-54945.iso --eu-wii-iso roms/RDSPAF.rvz --rebuild
```

This requires Dolphin's `dolphin-tool`; `--dolphin-tool PATH` selects an executable for either command. The
installer writes the Wii assets to `gamedata/dragon-net-battle/` after the build. An already extracted
`wzs3eu1.afs` or `wzs3us1.afs` can be given instead; pass the matching `wzs3eu2.afs` or `wzs3us2.afs`
with `--voice-afs PATH` to include the dialogue voices. Without it, an AFS input still extracts text and lip tracks.
With an ISO or RVZ input, the script extracts the second archive automatically. The script reads the archive's filename table, decodes
the DragonNe pack's BPE sections, writes native resource binaries and the GX I4/C4/C8 sheets as PNG, and makes
`gamedata/dragon-net-battle/index.html` for inspection. All output is ignored by Git. The GX palettes use split G/R and A/B
planes; the extractor reconstructs RGBA colors and the correct width and height from the texture headers.

The top-level Wii menu uses the extracted color art directly: eight landscape tiles, Pan, Giru, clouds, crest,
title, radar, bars, and both states of the three option labels. The cursor selects the original gold or blue
bar and its matching label. `home-motion.bin` holds keyframes decoded from the Wii FOD movie: the entrance of
the rows, Pan and Giru, Pan's idle movement, the radar pulse, and the lake sparkles. The home screen plays the complete
500-frame Pan motion loop at three-quarter speed with interpolation between Wii keyframes, so its final height meets
its first height without a jump; Giru floats on the same curve with a different phase. The title and crest enter
after the fade begins, using a longer slide and fade so their movement remains visible, and then drift gently.
The radar glides to the selected row, and Giru's artwork is mirrored across its vertical axis with its visible
bounds kept in place. The radar
pulse animates independently. These are whole-sprite movements;
the Wii source artwork does not provide articulated body frames for this home pose. A built-in motion fallback keeps them moving
if an older extraction has no `home-motion.bin`; the renderer logs which source it loaded. Selection fades through the
movie's five-frame highlight transition. The European archive has five DragonNe dialogue banks, decoded to
`home-subtitles-{en,es,de,fr,it}.bin` with 44 lines each. The port's selected menu language chooses the subtitle bank;
other menu languages fall back to English. Section 0 resource 6 has
separate Japanese and English lip events for each. The matching ADX voices are entries 30787–30830 (Japanese) and
32159–32202 (English) in the Wii voice archive. The extractor pairs them in `home-dialog.bin` and
`voices/{ja,en}/000.adx` through `043.adx`, checking each recording against its own lip track.
The game's Sound Options voice choice selects the matching recording and lip track independently of subtitle language.
Pan's base pose has open eyes; its original closed-eye overlay blinks briefly. After the
first cursor movement she uses the Wii pointing pose with its own eye and mouth sprites. The Wii lip track gates the
mouth animation for either pose: the body texture supplies the widest mouth, the first overlay is half open, and
the second overlay is closed. The recorded lip track chooses speaking or a pause; the two speaking shapes alternate
while it is open. Giru's dim eye is used while idle and his bright eye follows his own lip track.
The home scene speaks Pan's introduction and Giru's reply. Moving the cursor speaks the original description of the
selected option. The lower Wii menu pages use their original spoken descriptions too. Pan's mouth and Giru's eye
variants come from the same Wii art pack and change on their recorded lip events. The existing game message window
displays the extracted line. A screenshot is useful
for comparison but is not loaded by the port. Lower Wii menu pages currently use the port's drawn UI over the
decoded landscape.
