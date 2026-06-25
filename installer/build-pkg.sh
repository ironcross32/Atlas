#!/bin/zsh
set -euo pipefail
export COPYFILE_DISABLE=1

script_dir=${0:a:h}
repo_root=${script_dir:h}
version=${1:-$(/usr/bin/sed -nE 's/^project\(Atlas VERSION ([^ ]+).*/\1/p' "$repo_root/CMakeLists.txt" | /usr/bin/head -n 1)}

if [[ -z "$version" ]]; then
  echo "Unable to determine package version." >&2
  exit 1
fi

artifacts="$repo_root/build-release/Atlas_artefacts/Release"
au_bundle="$artifacts/AU/Atlas.component"
vst3_bundle="$artifacts/VST3/Atlas.vst3"

for required in "$au_bundle" "$vst3_bundle"; do
  if [[ ! -d "$required" ]]; then
    echo "Missing Release artifact: $required" >&2
    echo "Build first with: cmake --build build-release --target Atlas_All -j2" >&2
    exit 1
  fi
done

work_dir=$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/atlas-pkg.XXXXXX")
package_dir="$work_dir/packages"
out_dir="$repo_root/dist"
output_pkg="$out_dir/Atlas-${version}.pkg"

if [[ -e "$output_pkg" ]]; then
  output_pkg="$out_dir/Atlas-${version}-$(/bin/date +%Y%m%d%H%M%S).pkg"
fi

/bin/mkdir -p "$package_dir" "$out_dir"
/bin/mkdir -p "$work_dir/au-root" "$work_dir/vst3-root"
/bin/mkdir -p "$work_dir/docs-root/Library/Application Support/Atlas"

/usr/bin/ditto --noextattr --noqtn "$au_bundle" "$work_dir/au-root/Atlas.component"
/usr/bin/ditto --noextattr --noqtn "$vst3_bundle" "$work_dir/vst3-root/Atlas.vst3"
/bin/cp "$repo_root/installer/accessible-layout-guide.html" "$work_dir/docs-root/Library/Application Support/Atlas/Accessible Layout Guide.html"

/usr/bin/xattr -cr "$work_dir/docs-root" "$work_dir/au-root" "$work_dir/vst3-root" 2>/dev/null || true

/usr/bin/pkgbuild --root "$work_dir/docs-root" \
  --scripts "$repo_root/installer/pkg-scripts" \
  --identifier "plugins.alessio.atlas.docs" \
  --version "$version" \
  --install-location "/" \
  "$package_dir/Atlas-Docs.pkg"

/usr/bin/pkgbuild --root "$work_dir/au-root" \
  --identifier "plugins.alessio.atlas.au" \
  --version "$version" \
  --install-location "/Library/Audio/Plug-Ins/Components" \
  "$package_dir/Atlas-AU.pkg"

/usr/bin/pkgbuild --root "$work_dir/vst3-root" \
  --identifier "plugins.alessio.atlas.vst3" \
  --version "$version" \
  --install-location "/Library/Audio/Plug-Ins/VST3" \
  "$package_dir/Atlas-VST3.pkg"

/usr/bin/sed "s/@VERSION@/$version/g" "$repo_root/installer/Distribution.xml" > "$work_dir/Distribution.xml"

/usr/bin/productbuild --distribution "$work_dir/Distribution.xml" \
  --package-path "$package_dir" \
  "$output_pkg"

echo "$output_pkg"
echo "Temporary packaging workspace: $work_dir"
