on run argv
    if (count of argv) is not 1 then error "Expected the mounted volume name"

    set volumeName to item 1 of argv

    tell application "Finder"
        set backgroundImage to file ".background:dmg-background.png" of disk volumeName

        tell disk volumeName
            open
            delay 1

            set dmgWindow to container window
            set current view of dmgWindow to icon view
            set toolbar visible of dmgWindow to false
            set statusbar visible of dmgWindow to false
            set pathbar visible of dmgWindow to false
            set bounds of dmgWindow to {120, 120, 780, 520}

            set viewOptions to icon view options of dmgWindow
            set arrangement of viewOptions to not arranged
            set icon size of viewOptions to 128
            set text size of viewOptions to 13
            set label position of viewOptions to bottom
            set shows item info of viewOptions to false
            set shows icon preview of viewOptions to true
            set background picture of viewOptions to backgroundImage

            set position of item "Seed Atlas.app" to {170, 205}
            set position of item "Applications" to {490, 205}

            update without registering applications
            delay 2
            close dmgWindow
            delay 1
        end tell
    end tell
end run
