#!/bin/bash
# Builds the macOS installer (a .pkg) from built plugins. Runs on macOS (GitHub Actions builds it for every release).
#   installer/macos/build-pkg.sh <plugin dir with .vst3/.component bundles> <version> <output .pkg>
#
# The installer puts every plugin into the system-wide folders, so all users of the Mac see them:
#   /Library/Audio/Plug-Ins/VST3        (Cubase, Ableton Live, FL Studio, Reaper)
#   /Library/Audio/Plug-Ins/Components  (Logic Pro, GarageBand)
# with one tick box per plugin. Files written by Apple's Installer don't get the download quarantine flag, so
# the unsigned plugins load without the Terminal step a zip download needs; only the .pkg itself has to be
# allowed once in System Settings > Privacy & Security.
set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <plugin dir> <version> <output .pkg>" >&2
    exit 1
fi

plugin_dir=$(cd "$1" && pwd)
version=$2
output=$3
here=$(cd "$(dirname "$0")" && pwd)
licence="$here/../../LICENSE"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/packages" "$work/resources"

describe() {
    case "$1" in
        DynMap)      echo "Multiband dynamics you draw: level and transient curves on up to 12 bands, saturation, clipper and true-peak limiter." ;;
        FloorMatch)  echo "Brings the background noise of dialogue takes to one level and colour, leaving the dialogue untouched." ;;
        MSC)         echo "Multistage stereo control: band split, dynamic pan, Haas, Juno-style chorus, mid/side image." ;;
        StereoScale) echo "Left / right / mid / side gain, 0-200 %." ;;
        WaveRef)     echo "Reference A/B: loaded tracks, loudness match, mid/side spectrum, host sync with time-stretch." ;;
        LavAlign)    echo "Standalone app (in Applications): phase-aligns lavalier mics to the boom in recorder files." ;;
        *)           echo "$1 by Maki Plugins." ;;
    esac
}

choices=""
outline=""
found=0

for vst3 in "$plugin_dir"/*.vst3; do
    [ -d "$vst3" ] || continue
    name=$(basename "$vst3" .vst3)
    id=$(echo "$name" | tr '[:upper:]' '[:lower:]')
    pkg_id="com.makiplugins.pkg.$id"
    found=1

    # Payload: the plugin's bundles laid out as they go under /Library/Audio/Plug-Ins.
    root="$work/roots/$name"
    mkdir -p "$root/VST3"
    ditto "$vst3" "$root/VST3/$name.vst3"
    if [ -d "$plugin_dir/$name.component" ]; then
        mkdir -p "$root/Components"
        ditto "$plugin_dir/$name.component" "$root/Components/$name.component"
    fi

    # By default Installer "relocates" a bundle to wherever a copy with the same bundle ID already sits
    # (e.g. one copied by hand into ~/Library), which would leave the system folder without it.
    plist="$work/$id-components.plist"
    pkgbuild --analyze --root "$root" "$plist" > /dev/null
    i=0
    while /usr/libexec/PlistBuddy -c "Print :$i" "$plist" > /dev/null 2>&1; do
        plutil -replace "$i.BundleIsRelocatable" -bool NO "$plist"   # adds the key when pkgbuild left it out
        i=$((i + 1))
    done

    # preinstall removes the old version first, so no stale files are left inside the bundles;
    # postinstall clears any quarantine flag and makes macOS re-read its list of Audio Units.
    scripts="$work/scripts/$name"
    mkdir -p "$scripts"
    cat > "$scripts/preinstall" <<EOF
#!/bin/bash
rm -rf "\$2/VST3/$name.vst3" "\$2/Components/$name.component"
exit 0
EOF
    cat > "$scripts/postinstall" <<EOF
#!/bin/bash
xattr -dr com.apple.quarantine "\$2/VST3/$name.vst3" "\$2/Components/$name.component" 2> /dev/null
killall -9 AudioComponentRegistrar 2> /dev/null
exit 0
EOF
    chmod +x "$scripts/preinstall" "$scripts/postinstall"

    pkgbuild --root "$root" \
             --component-plist "$plist" \
             --scripts "$scripts" \
             --identifier "$pkg_id" \
             --version "$version" \
             --install-location /Library/Audio/Plug-Ins \
             "$work/packages/$id.pkg"

    outline+="        <line choice=\"$id\"/>"$'\n'
    choices+="    <choice id=\"$id\" title=\"$name\" description=\"$(describe "$name")\">"$'\n'
    choices+="        <pkg-ref id=\"$pkg_id\"/>"$'\n'
    choices+="    </choice>"$'\n'
    choices+="    <pkg-ref id=\"$pkg_id\" version=\"$version\" onConclusion=\"none\">$id.pkg</pkg-ref>"$'\n'
done

# Standalone apps (LavAlign) go to /Applications.
for app in "$plugin_dir"/*.app; do
    [ -d "$app" ] || continue
    name=$(basename "$app" .app)
    id=$(echo "$name" | tr '[:upper:]' '[:lower:]')
    pkg_id="com.makiplugins.pkg.$id"

    root="$work/roots/$name"
    mkdir -p "$root"
    ditto "$app" "$root/$name.app"
    plist="$work/$id-components.plist"
    pkgbuild --analyze --root "$root" "$plist" > /dev/null
    i=0
    while /usr/libexec/PlistBuddy -c "Print :$i" "$plist" > /dev/null 2>&1; do
        plutil -replace "$i.BundleIsRelocatable" -bool NO "$plist"
        i=$((i + 1))
    done
    scripts="$work/scripts/$name"
    mkdir -p "$scripts"
    cat > "$scripts/preinstall" <<EOF
#!/bin/bash
rm -rf "\$2/$name.app"
exit 0
EOF
    cat > "$scripts/postinstall" <<EOF
#!/bin/bash
xattr -dr com.apple.quarantine "\$2/$name.app" 2> /dev/null
exit 0
EOF
    chmod +x "$scripts/preinstall" "$scripts/postinstall"
    pkgbuild --root "$root" \
             --component-plist "$plist" \
             --scripts "$scripts" \
             --identifier "$pkg_id" \
             --version "$version" \
             --install-location /Applications \
             "$work/packages/$id.pkg"

    outline+="        <line choice=\"$id\"/>"$'\n'
    choices+="    <choice id=\"$id\" title=\"$name (app)\" description=\"$(describe "$name")\">"$'\n'
    choices+="        <pkg-ref id=\"$pkg_id\"/>"$'\n'
    choices+="    </choice>"$'\n'
    choices+="    <pkg-ref id=\"$pkg_id\" version=\"$version\" onConclusion=\"none\">$id.pkg</pkg-ref>"$'\n'
done

if [ $found -eq 0 ]; then
    echo "No .vst3 bundles in $plugin_dir" >&2
    exit 1
fi

cp "$licence" "$work/resources/LICENSE.txt"

cat > "$work/resources/welcome.html" <<EOF
<html><body style="font-family: -apple-system, Helvetica; font-size: 13px">
<h2>Maki Plugins $version</h2>
<p>Free audio plugins, VST3 and Audio Unit, for Apple Silicon and Intel Macs (macOS 11 or newer).</p>
<p>They are installed for every user of this Mac:</p>
<ul>
<li><b>Audio Unit</b> (Logic Pro, GarageBand): /Library/Audio/Plug-Ins/Components</li>
<li><b>VST3</b> (Cubase, Ableton Live, FL Studio, Reaper): /Library/Audio/Plug-Ins/VST3</li>
<li><b>LavAlign</b> (standalone app, aligns lav mics to the boom): /Applications</li>
</ul>
<p><b>Please quit your DAW before continuing.</b> Running the installer again updates the plugins;
click <i>Customize</i> on the Installation Type page to pick which ones to install.</p>
</body></html>
EOF

cat > "$work/resources/conclusion.html" <<EOF
<html><body style="font-family: -apple-system, Helvetica; font-size: 13px">
<h2>Done</h2>
<p>Open your DAW: the plugins are listed under <b>Maki Plugins</b>.</p>
<ul>
<li><b>Logic Pro</b>: if a plugin is missing, open Logic Pro &gt; Settings &gt; Plug-in Manager, select it and click
<i>Reset &amp; Rescan Selection</i>.</li>
<li><b>Ableton Live</b>: Settings &gt; Plug-ins, switch on <i>Use VST3 Plug-in System Folders</i>, then Rescan.</li>
<li><b>FL Studio</b>: Options &gt; Manage plugins &gt; Find installed plugins.</li>
<li><b>Cubase</b> and <b>Reaper</b> find them on the next start.</li>
</ul>
</body></html>
EOF

cat > "$work/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Maki Plugins $version</title>
    <welcome file="welcome.html" mime-type="text/html"/>
    <license file="LICENSE.txt"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <volume-check>
        <allowed-os-versions><os-version min="11.0"/></allowed-os-versions>
    </volume-check>
    <choices-outline>
$outline    </choices-outline>
$choices</installer-gui-script>
EOF

mkdir -p "$(dirname "$output")"
productbuild --distribution "$work/distribution.xml" \
             --package-path "$work/packages" \
             --resources "$work/resources" \
             --version "$version" \
             "$output"

echo "Built $output"
