- **Clean uninstall.** Erasing MiniAmp in Mixtape (0.1.5 or later) now stops the background music and puts OnionOS's original sound library back if Music in games was on. Before, erasing MiniAmp left its sound wrapper in place. Games still sounded normal, but OnionOS's library was left modified.
- New `miniamp --uninstall`, run by `mixtape-uninstall.sh`, does the same thing by hand.

If you're on 0.2.0, update MiniAmp before erasing it, or turn Options > Music in games off first.
