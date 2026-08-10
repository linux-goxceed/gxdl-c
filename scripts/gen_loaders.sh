#!/bin/sh
set -eu

if [ "$#" -lt 3 ]; then
    echo "usage: gen_loaders.sh OUTPUT_DIR BIN2C LOADER..." >&2
    exit 2
fi

output_dir=$1
bin2c=$2
shift 2

mkdir -p "$output_dir"
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

registry="$temporary/embedded_registry.c"
{
    printf '#include "gxdl.h"\n\n'
    printf '#include <string.h>\n\n'
} > "$registry"

entries="$temporary/entries"
: > "$entries"

for loader in "$@"; do
    filename=${loader##*/}
    stem=${filename%.boot}
    symbol=$(printf '%s' "$stem" | sed 's/[^A-Za-z0-9_]/_/g')
    symbol="gx_loader_${symbol}"
    size=$(wc -c < "$loader" | tr -d '[:space:]')
    generated="$temporary/loader_${stem}.c"
    "$bin2c" "$symbol" < "$loader" > "$generated"
    {
        printf 'extern const char %s[];\n' "$symbol"
    } >> "$registry"
    printf '    {"%s", (const unsigned char *)%s, %sU},\n' \
        "$stem" "$symbol" "$size" >> "$entries"
done

{
    printf '\nstatic const gx_embedded_loader loaders[] = {\n'
    sed -n '1,$p' "$entries"
    printf '};\n\n'
    printf 'size_t gx_embedded_loader_count(void) {\n'
    printf '    return sizeof(loaders) / sizeof(loaders[0]);\n}\n\n'
    printf 'const gx_embedded_loader *gx_embedded_loader_at(size_t index) {\n'
    printf '    return index < gx_embedded_loader_count() ? &loaders[index] : NULL;\n}\n\n'
    printf 'const gx_embedded_loader *gx_embedded_loader_find(const char *name) {\n'
    printf '    size_t i;\n'
    printf '    for (i = 0; i < gx_embedded_loader_count(); ++i)\n'
    printf '        if (strcmp(loaders[i].name, name) == 0) return &loaders[i];\n'
    printf '    return NULL;\n}\n'
} >> "$registry"

for generated in "$temporary"/loader_*.c "$registry"; do
    destination="$output_dir/${generated##*/}"
    if [ ! -f "$destination" ] || ! cmp -s "$generated" "$destination"; then
        cp "$generated" "$destination"
    fi
done

for old in "$output_dir"/loader_*.c; do
    [ -e "$old" ] || continue
    [ -e "$temporary/${old##*/}" ] || rm -f "$old"
done
