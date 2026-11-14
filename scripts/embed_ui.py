from pathlib import Path
import re
import gzip

Import("env")
project = Path(env.subst("$PROJECT_DIR"))
version = (project / "VERSION").read_text(encoding="utf-8").strip()
if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+(?:[A-Za-z][A-Za-z0-9]*)?(?:-[A-Za-z0-9.-]+)?", version):
    raise ValueError("VERSION deve conter apenas uma versão, por exemplo 0.2b")
source = (project / "web/index.html").read_text(encoding="utf-8")
source = source.replace("__FIRMWARE_VERSION__", version)
compressed = gzip.compress(source.encode("utf-8"), compresslevel=9, mtime=0)
compressed_literal = ",".join(str(value) for value in compressed)
gallery = (project / "web/gallery.html").read_text(encoding="utf-8")
gallery = gallery.replace("__FIRMWARE_VERSION__", version)
gallery_compressed = gzip.compress(gallery.encode("utf-8"), compresslevel=9, mtime=0)
gallery_literal = ",".join(str(value) for value in gallery_compressed)
folder_actions = (project / "web/folder_actions.js").read_bytes()
folder_actions_gzip = gzip.compress(folder_actions, compresslevel=9, mtime=0)
folder_actions_literal = ",".join(str(value) for value in folder_actions_gzip)
services_script = (project / "web/services.js").read_bytes()
services_gzip = gzip.compress(services_script, compresslevel=9, mtime=0)
services_literal = ",".join(str(value) for value in services_gzip)
outputs = {
    project / "src/web_ui.h": (
        '#pragma once\n#include <pgmspace.h>\n'
        'static const char WEB_UI[] PROGMEM = R"REMOTECAM_UI('
        + source + ')REMOTECAM_UI";\n'
        + 'static const unsigned char WEB_UI_GZIP[] PROGMEM = {'
        + compressed_literal + '};\n'
        + 'static const unsigned char GALLERY_UI_GZIP[] PROGMEM = {'
        + gallery_literal + '};\n'
        + 'static const unsigned char FOLDER_ACTIONS_GZIP[] PROGMEM = {'
        + folder_actions_literal + '};\n'
        + 'static const unsigned char SERVICES_UI_GZIP[] PROGMEM = {'
        + services_literal + '};\n'
    ),
    project / "src/firmware_version.h": (
        '#pragma once\n#define FIRMWARE_VERSION "' + version + '"\n'
    ),
}
for target, contents in outputs.items():
    if not target.exists() or target.read_text(encoding="utf-8") != contents:
        target.write_text(contents, encoding="utf-8")
