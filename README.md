<div align="center">

# REKANGED!
<img width="821" height="245" alt="logo" src="https://github.com/user-attachments/assets/07615886-71e6-4d74-b1a8-9eae8229643d" />

### An unofficial True Crime: Streets of LA PC Port made by recompiling the Original Xbox version and providing full modern PC options, as well as new content!

This port is made using [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) Tool

<sub><b>No game files included</b>: You need to provide your legally owned True Crime: Streets of LA European (PAL) Xbox Redump, in order for the recompilation to run. <b>Linux support is still on hold</b>: I do not have and use Linux unfortunately. Unless someone contributes to port and test it on Linux, This is Windows only for now</sub>

</div>

<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/9773a81b-04ab-4e54-ba4b-e80dc162a203" />

## Supported Versions

| Build | Disc | Status | default.xbe SHA-1 |
|---|---|---|---|
| 4156000E Europe (PAL) | Xbox DVD, En/Fr/De/Es/It, XDK 5659 | Base game, plays on its own | `7c665c36…4456` (iso) or `5e0aeb15…2d7b` (xiso)  |
| 4156000E Europe (PAL), English-only disc | Xbox DVD, En, XDK 5659 | Base game, plays on its own | `ca906ba5…d2ed` (iso) or `19aa3175…5505` (xiso) |
| USA (NTSC) | not checked | Not supported | – |
| Japan | not checked | Not supported | – |

A full disc dump or a trimmed xiso both work, since only the `default.xbe` inside is checked.

## How To Use

**Requirements**:
(Setup)
- Windows 10/11 64-bit, GPU with Direct3D 12 
- Your own supported Xbox disc image of the game (See above for supported versions)
(Building)
- Windows 10/11 64-bit, GPU with Direct3D 12 
- Your own supported Xbox disc image of the game (See above for supported versions)
- Visual Studio 2022 with Desktop development with C++ (`build.bat` offers to add the Clang and CMake Tools)
- Git for Windows and an internet connection for the first build

**Setup**:
1. Download the latest archive from [Releases](https://github.com/technicrain0/Streets-of-LA-Recomp/releases) and extract it.
2. Put your chosen redump (`.iso`) or extracted xbe with your data folder beside it (`Data` folder and `default.xbe`) into the `assets` folder
3. Run `TrueCrimeLA.exe` 
4. The tool will detect your files. After that, Press `"Set Up and Play"`
5. Wait until it compiles
6. Press `A` after it says "Start the Game Now?" and Play!

**Building**:
1. Download the Source Code or use "git clone https://github.com/technicrain0/ReKANGED" on a chosen folder
2. Create an `assets` folder and put your chosen redump (`.iso`) or extracted xbe with your data folder beside it (`Data` folder and `default.xbe`)
3. Run `build.bat`
4. Wait until its finished and it will eventually say `"Play Now? "`
5. Press Y
6. Your build will be in "`out\build\release\`"
(To Convert it to a working exe. Use `release.bat`)

## Installing Optional Content
**PC Content**
1. Provide your own copy of the LTI Gray Matter True Crime PC Port
2. On Setup, Click Browse and link your PC Copy to the path or type it in manually
3. Press "Set Up and Play"

**Makoto Yuki**
1. Provide your own USA copy of Persona 3 FES (2006) (for Makoto Yuki) and put it in the `assets` folder
2. Press "Set Up and Play"

## Screenshots
<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/d179c0fa-505d-470a-a277-25970427c021" />
<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/9ef02fbf-2500-4dc3-8a8b-461a1b088683" />
<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/c2158a3e-06ac-454f-87a8-5ef582ed94ee" />

## Features
`New Skins:`
<img width="2000" height="1120" alt="SKIN SHOWCASE" src="https://github.com/user-attachments/assets/449b965b-cbe1-469a-abed-79633f4d2028" />
- **PC Skins** - Jeanette, Rikimaru, COD Soldier, Taurus, Ayame, Desperado, Officer Dick, Sandwich and Snoop Dogg
- **Makoto Yuki** - The Protagonist of Persona 3 (Needs a copy of a US Persona 3 FES iso)

`Notable Features:`
- **PC Exclusive Content** - Music Tracks like Static X - The Only, as well as skins for now
- **Native Widescreen** – the game’s own HD layout, which PAL consoles never allowed, now runs on the European release. Though you can switch between Original Stretched and True Widescreen
- **Display Mode** - Option to switch between: *Windowed*, *Fullscreen*, *Borderless*.
- **FPS Option** - The original game's aim is 60 FPS. but in here, you can switch between 30 and 60 FPS whenever you want. Kinda obsolete? but in case for low end glitches
- **Custom Resolution** - Now you can switch between: *800x600*, *1024x768*, *1280x720*, *1600x1200*, *1920x1080*
- **An Exit Button in the Menu** - *An exit button..* ....*an exit button.. yes*

`Plans:`
- **Multiplayer** - Hard to Implement but PC has it so its planned
- **New Skins** 
- **Improved Draw Distance and Rendering**
- **Raw Mouse and Keyboard Input** - An ability to play with Keyboard and Mouse properly just like the official PC Port. Would take alot of time
- **Controller Preset 3** - My inputs. a more modernized control scheme
- **Camera Tweaks** - Fixing the ridiculous R Stick camera by implementing a function similar to the 3D Era Grand Theft Auto Games
- **Gammas/Filters** - Switching between Xbox's default darker atmosphere, PS2's more blurry aesthetic or PC's cleaner and yellowish look
- **Free Aim** - Similar to the Official PC Port. Can be applied if Raw KBM is implemented
- **Japanese Dub** - From the Japanese ISO
- **Support For Multiple Xisos and Isos** - For Accessibility

## To Fix
- Snoop Dogg (ACTVZZL) Bugs - Misplaced Eyeball and Arm Texture
- Desperado (RECKON) Bugs - Misplaced Eyeballs

## Credits
- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) - Recompiler Tool
- [RDR-XBERecomp](https://github.com/TGP482/RDR-XBERecomp) - For References
