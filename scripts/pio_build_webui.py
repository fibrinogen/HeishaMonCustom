Import("env")

import os
import runpy

runpy.run_path(os.path.join(env.subst("$PROJECT_DIR"), "scripts", "build_webui_package.py"), run_name="__main__")
