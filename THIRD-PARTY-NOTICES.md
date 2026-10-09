# Third-party notices

- nlohmann/json 3.12.0: Niels Lohmann and contributors, MIT license. CMake fetches the official release archive and verifies SHA-256. The full notice is in `docs/LICENSE-nlohmann-json.txt`. It is not a Ymir dependency.
- `config/jp-area-codes.json`: current Japanese fixed-line regional-display mapping derived from the NTT East/West publications cited in the JSON. This is not a reconstruction of historical XBAND local-call boundaries or a subscriber list.
- References to X-Band/SegaServer and SegaOS identify research sources only. Those repositories' source files are not copied into this release. XBAND icon images are not bundled.
- `components/xband/adapters/ymir` contains this project's adapter examples. Ymir itself, its upstream source, executable, BIOS and game media are not included.
- `integration/ymir/ymir-9a237ea-serial-xband.patch` contains integration changes and limited upstream source context for Ymir commit `9a237ea6642912ae0833f691809aa47dc27f908d`. It is not the complete Ymir source or executable. Upstream GPLv3 license text is preserved in `integration/ymir/LICENSE-GPL-3.0.txt`; upstream and component notices must remain with their respective materials.

This private snapshot does not assign a new license to third-party materials or claim rights to SEGA/Catapult trademarks. A public release would require a separate licensing review of the new code and any derived portions.
