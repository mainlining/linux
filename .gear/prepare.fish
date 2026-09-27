#!/usr/bin/fish

set -l spec_path .gear/kernel-image.spec

argparse 'h/help' 'n/name=' 'r/release=' -- $argv

if set -ql _flag_h; or not set -ql _flag_n
    echo "-h | --help"\t"Show this message" >&2
    echo "-n | --name"\t"Set kernel image name" >&2
    echo "-r | --release"\t"Set kernel release (1 by default)" >&2
    return 1
end

if not set -ql _flag_r
        echo "Using release = 1"
        set -g _flag_r 1
end


function get_nextver
        if test -f localversion-next
                set -l lc (string trim -- (cat localversion-next))
                set -l date (string match -r -- '.*-next-([0-9]{8}).*' "$lc" | head -n1)
                if test -n "$date"
                        if test "$date" = "$lc"
                                set date (echo "$lc" | sed -n 's/.*-next-\([0-9]\{8\}\).*/\1/p' | head -n1 | string trim)
                        end
                        echo ".$date"
                end
        end
end

set -l VERSION     (sed -n 's/^VERSION[[:space:]]*=[[:space:]]*//p' Makefile | head -n1 | string trim)
set -l PATCHLEVEL  (sed -n 's/^PATCHLEVEL[[:space:]]*=[[:space:]]*//p' Makefile | head -n1 | string trim)
set -l SUBLEVEL    (sed -n 's/^SUBLEVEL[[:space:]]*=[[:space:]]*//p' Makefile | head -n1 | string trim)
set -l EXTRAVERSION (sed -n 's/^EXTRAVERSION[[:space:]]*=[[:space:]]*//p' Makefile | head -n1 | string trim)

set -l EXTRAVERSION (string replace -r -- '^-+' '.' "$EXTRAVERSION")
set -l EXTRAVERSION "$EXTRAVERSION$(get_nextver)"
if test -z "$EXTRAVERSION"
    set EXTRAVERSION "%nil"
end

sed -i "s|@@rel@@|$_flag_r|g" $spec_path
sed -i "s|@@flav@@|$_flag_n|g" $spec_path
sed -i "s|@@basever@@|$VERSION.$PATCHLEVEL|g" $spec_path
sed -i "s|@@sublvl@@|.$SUBLEVEL|g" $spec_path
sed -i "s|@@extraver@@|$EXTRAVERSION|g" $spec_path

add_changelog -e "- New verison" $spec_path

echo "Spec patched"
echo "Don't forget to add previous changelogs!"

gear-store-tags -ac
git add -f .gear
