# External Display — install and first start

External Display makes a Windows PC a second screen for a Mac, over Wi-Fi. You need:

- a Mac with macOS 13 (Ventura) or newer;
- a Windows 10 or 11 PC (64-bit);
- both on the same network (not a guest Wi-Fi).

There are two downloads: **ExternalDisplay-….dmg** for the Mac, **ExternalDisplayReceiver-….zip**
for the PC. You don't need Xcode or Visual Studio.

## On the Mac

1. Open the `.dmg` and drag **External Display** onto **Applications**.
2. Open it from Applications. The app is not signed by a registered Apple developer, so macOS
   stops it the first time:
   - Click **Done** (not "Move to Trash").
   - Open System Settings → Privacy & Security, scroll down, and click **Open Anyway** next to
     "External Display". Confirm with your password, then **Open**.

   This is needed only once. (Or, in Terminal:
   `xattr -dr com.apple.quarantine "/Applications/External Display.app"`.)
3. Allow what macOS asks for:
   - **Screen Recording** (System Settings → Privacy & Security → Screen & System Audio
     Recording): turn on External Display, then quit and reopen the app.
   - **Accessibility** (same place → Accessibility): lets the PC's mouse and keyboard work on the
     Mac.
   - **Local Network**: allow it, so the PC can find the Mac.
4. In the app: choose a resolution (1920×1080 is a good start), click **Create Virtual
   Display**, then **Start Capture**.

## On the PC

1. Unzip the folder somewhere of your own, such as Documents. Not in Program Files: the app saves
   its settings next to itself.
2. Double-click **ExternalDisplayReceiver.exe**.
   - Windows may say "Windows protected your PC": click **More info**, then **Run anyway**.
   - Windows Firewall asks about networks: allow **Private networks**.
3. The PC finds the Mac by itself.

## Pair them (once)

1. The Mac shows a 6-digit code.
2. Type it on the PC.
3. Click **Pair** on the Mac.

From then on they connect without a code.

## Using it

- On the PC: **F11** switches between full screen and a window, **F2** shows the statistics,
  **Esc** quits.
- On the Mac, **Quality** chooses Battery Saver, Balanced or High. High streams 120 frames a
  second if the PC's screen runs at 120 Hz.
- The virtual display is arranged like any monitor: System Settings → Displays.

## For the smoothest picture

- Use 5 GHz Wi-Fi, close to the router, or a cable on one of the two machines.
- On the Mac, while you use it: set AirDrop to **No One** and turn off **Handoff** (System
  Settings → General → AirDrop & Handoff). Both make the Wi-Fi pause about once a second.

## If something does not work

- **The PC does not find the Mac:** both must be on the same network, and not a guest Wi-Fi. The
  Mac app must show "Listening". Windows' network must be set to *Private*.
- **No picture:** click **Start Capture** on the Mac. Screen Recording must be allowed (step 3
  above), and the app reopened afterwards.
- **The mouse does nothing on the Mac:** allow Accessibility (step 3 above).
- **To report a problem**, send these two files:
  - the PC's `receiver.log`, in the folder with the .exe;
  - the Mac's `sender.log` (in the app's menu: 🔍 → Show Log).
