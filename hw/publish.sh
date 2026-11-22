#!/bin/sh
# Publishes what hw/run.sh has written to hw-out/: a directory of its own on
# the branch hw-results of the fork, which is read from elsewhere later.
#
#   sh hw/publish.sh              on Linux, or in Git Bash on Windows
#   HW_TAG=pc2 sh hw/publish.sh   another name for the machine than "pc1"
#
# It needs git and the right to push to the fork.  It commits as "popolupo",
# whatever git is set up with on this machine, and refuses a result which has
# the name of this machine or of its user in it.  Nothing but the text files
# in hw-out/ leaves the machine.
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
out="$top/hw-out"
res="$top/hw-results"
tag=${HW_TAG:-pc1}

ls "$out"/*.txt > /dev/null 2>&1 || { echo "There is nothing in hw-out/ to publish. Run: sh hw/run.sh"; exit 1; }
case "$tag" in
    *[!a-z0-9-]*|'') echo "HW_TAG may have small letters, digits and dashes"; exit 1 ;;
esac

# Nothing which names this machine or its user.
for word in "${USER:-}" "${USERNAME:-}" "$(id -un 2> /dev/null)" "$(hostname 2> /dev/null)" "${COMPUTERNAME:-}"; do
    [ ${#word} -ge 3 ] || continue
    if grep -q -i -w -F -- "$word" "$out"/*.txt; then
        echo "A result in hw-out/ has the name of this machine or of its user in it. Nothing was published."
        exit 1
    fi
done

url=$(git -C "$top" config --get remote.origin.url)
[ -n "$url" ] || { echo "This is not a clone with a remote called origin"; exit 1; }
if [ -d "$res/.git" ]; then
    git -C "$res" pull -q --ff-only || { echo "The clone in hw-results/ cannot be brought up to date"; exit 1; }
else
    git clone -q --single-branch --branch hw-results --depth 1 "$url" "$res" \
        || { echo "The branch hw-results cannot be fetched from $url"; exit 1; }
fi
git -C "$res" config user.name popolupo
git -C "$res" config user.email 4758069+MarquisDeGris@users.noreply.github.com

stamp=$(date -u +%Y%m%d-%H%M%S)
dir="results/$stamp-$tag"
mkdir -p "$res/$dir" && cp "$out"/*.txt "$res/$dir/" || exit 1
git -C "$res" add "$dir" || exit 1
git -C "$res" commit -q -m "results: $stamp $tag" || exit 1
if git -C "$res" push -q origin HEAD:hw-results; then
    echo "Published as $dir on the branch hw-results."
else
    echo "The commit is made in hw-results/ and was not pushed: this machine has no right to push to the fork."
    echo "Copy the text of hw-out/*.txt to where it is read instead."
    exit 1
fi
