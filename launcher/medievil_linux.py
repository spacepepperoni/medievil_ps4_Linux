"""Linux fork (spacepepperoni/medievil_ps4_Linux): what the AppImage needs beyond upstream's
Linux build. Releases come from the fork, the AppImage updates itself (from the launcher, and
beside the game when Steam starts it with --play), and Add to Steam puts MediEvil in the Steam
library as a non-Steam game. Upstream files call in here at lines marked "Linux fork".
"""
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time
import urllib.request

REPO = 'spacepepperoni/medievil_ps4_Linux'
RELEASES_API = f'https://api.github.com/repos/{REPO}/releases/latest'
RELEASES_PAGE = f'https://github.com/{REPO}/releases'
ASSET = 'MediEvil-x86_64.AppImage'
STEAM_NAME = 'MediEvil'

# The build number CI writes beside this file (v<VERSION>-linux.<N>); empty in a source tree.
try:
    LINUX_BUILD = (Path(__file__).with_name('linux_build.txt')).read_text(encoding='utf-8').strip()
except OSError:
    LINUX_BUILD = ''

# The bundled Python's OpenSSL looks for certificates in one fixed place; point it at the
# system's bundle so the update check works on every distribution.
if not os.environ.get('SSL_CERT_FILE'):
    for _bundle in ('/etc/ssl/certs/ca-certificates.crt', '/etc/pki/tls/certs/ca-bundle.crt',
                    '/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem', '/etc/ssl/cert.pem',
                    '/etc/ssl/ca-bundle.pem'):
        if os.path.isfile(_bundle):
            os.environ['SSL_CERT_FILE'] = _bundle
            break


def version(upstream):
    return f'{upstream}-linux.{LINUX_BUILD}' if LINUX_BUILD else upstream


def appimage():
    path = os.environ.get('APPIMAGE')
    return Path(path) if path else None


def clean_env():
    """The environment for programs started outside the AppImage (Steam, a new AppImage)."""
    env = dict(os.environ)
    for name in ('APPIMAGE', 'APPDIR', 'ARGV0', 'OWD', 'LD_LIBRARY_PATH', 'PYTHONNOUSERSITE',
                 'PYTHONDONTWRITEBYTECODE', 'PYTHONUTF8', 'GOW3_PROBE', 'MEDIEVIL_FONT_DIR'):
        env.pop(name, None)
    return env


def install_appimage(url, target, progress=None):
    """Downloads the AppImage at `url` next to `target` and puts it in its place (a rename: the
    running AppImage keeps its own mounted copy until it closes)."""
    part = target.with_name(target.name + '.part')
    request = urllib.request.Request(url, headers={'User-Agent': 'medievil-linux-launcher'})
    try:
        with urllib.request.urlopen(request, timeout=60) as response, open(part, 'wb') as out:
            expected = int(response.headers.get('Content-Length') or 0)
            done, shown = 0, -1
            while chunk := response.read(1 << 20):
                out.write(chunk)
                done += len(chunk)
                percent = done * 100 // expected if expected else 0
                if progress and percent != shown:
                    shown = percent
                    progress(percent)
        size = part.stat().st_size
        with open(part, 'rb') as check:
            magic = check.read(4)
        if magic != b'\x7fELF' or size < (1 << 20) or (expected and size != expected):
            raise ValueError('the download is incomplete')
        part.chmod(0o755)
        os.replace(part, target)
    finally:
        part.unlink(missing_ok=True)


def update_in_background(latest_release, version_tuple, current, settings):
    """--play (Steam, Game Mode) never opens the launcher: the update check runs beside the game
    and the new AppImage is swapped in for the next start."""
    if not appimage() or not settings.get('update_check', True):
        return None

    def work():
        try:
            newest, url, _page = latest_release()
            if url and version_tuple(newest) > version_tuple(current):
                install_appimage(url, appimage())
                print(f'Update: version {newest} installed for the next start', flush=True)
        except (OSError, ValueError, KeyError):
            pass
    thread = threading.Thread(target=work, daemon=True)
    thread.start()
    return thread


def install_update(launcher, version_name, url, page, _, app_name):
    """The sidebar's Update button: replaces the AppImage, then opens the new one."""
    import webbrowser
    target = appimage()
    if not target or not url:  # a source tree updates with git
        webbrowser.open(page)
        return
    if not launcher.messagebox.askyesno(app_name, _(
            'Install version {} now? The launcher closes and opens again. Saves and settings are kept.')
            .format(version_name)):
        return
    launcher.update_button.configure(state='disabled')

    def progress(percent):
        launcher.ui_calls.put(lambda: launcher.update_label.configure(
            text=_('Downloading version {}… {}%').format(version_name, percent)))

    def restart():
        subprocess.Popen([str(target)], env=clean_env(), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)
        launcher.root.destroy()

    def work():
        try:
            install_appimage(url, target, progress)
            launcher.ui_calls.put(restart)
        except (OSError, ValueError) as failure:
            def failed(error=failure):
                launcher.update_button.configure(state='normal')
                launcher.update_label.configure(text=_('Version {} is available.').format(version_name))
                launcher.messagebox.showerror(app_name, _('Update failed: {}').format(error))
            launcher.ui_calls.put(failed)
    threading.Thread(target=work, daemon=True).start()


# ---- Add to Steam ---------------------------------------------------------------------------

def steam_folder():
    home = Path.home()
    for candidate in (home / '.steam' / 'steam', home / '.local' / 'share' / 'Steam',
                      home / '.var' / 'app' / 'com.valvesoftware.Steam' / '.local' / 'share' / 'Steam'):
        if (candidate / 'userdata').is_dir():
            return candidate.resolve()
    return None


def steam_running():
    result = subprocess.run(['pgrep', '-x', 'steam'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return result.returncode == 0


def steam_shutdown(steam):
    if 'com.valvesoftware.Steam' in str(steam):
        command = ['flatpak', 'run', 'com.valvesoftware.Steam', '-shutdown']
    elif shutil.which('steam'):
        command = [shutil.which('steam'), '-shutdown']
    else:
        return
    subprocess.Popen(command, env=clean_env(), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)


def steam_start():
    try:
        subprocess.Popen(['xdg-open', 'steam://open/games'], env=clean_env(), stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    except OSError:
        pass


def steam_command(steam, launcher_file):
    """(Exe, LaunchOptions) of the shortcut: the AppImage with --play, else this Python with the
    launcher script. Flatpak Steam starts host programs through flatpak-spawn --host."""
    import sys
    if appimage():
        exe, options = appimage(), '--play'
    else:
        exe, options = Path(sys.executable), f'"{Path(launcher_file).resolve()}" --play'
    if 'com.valvesoftware.Steam' in str(steam):
        return '/usr/bin/flatpak-spawn', f'--host "{exe}" {options}'
    return str(exe), options


def add_to_steam(launcher, launcher_file, config_dir, port_dir, _, app_name):
    """MediEvil in the Steam library, started with the settings saved in the launcher."""
    import bbport_steam
    if not (Path(launcher.app.get('game_dir') or '.') / 'eboot.bin').is_file():
        launcher.messagebox.showerror(app_name, _('Choose your game folder first (Game page).'))
        return
    steam = steam_folder()
    configs = bbport_steam.user_configs(steam) if steam else []
    if not configs:
        launcher.messagebox.showerror(app_name, _('Steam was not found on this PC (or nobody has signed in to it yet).'))
        return
    restart = False
    if steam_running():
        if not launcher.messagebox.askyesno(app_name, _('Steam has to close while MediEvil is added; it starts '
                                                         'again afterwards. Close Steam now?')):
            return
        steam_shutdown(steam)
        for _i in range(60):
            time.sleep(0.5)
            if not steam_running():
                break
        else:
            launcher.messagebox.showerror(app_name, _('Steam did not close. Close it yourself and try again.'))
            return
        time.sleep(1)
        restart = True
    # The icon lives outside the AppImage, whose files are gone once it closes.
    icon = Path(config_dir) / 'medievil.png'
    try:
        Path(config_dir).mkdir(parents=True, exist_ok=True)
        for source in (Path(launcher.app.get('game_dir') or '.') / 'sce_sys' / 'icon0.png',
                       Path(port_dir) / 'launcher' / 'medievil.png'):
            if source.is_file():
                shutil.copyfile(source, icon)
                break
    except OSError:
        pass
    exe, options = steam_command(steam, launcher_file)
    start_dir = Path(os.environ.get('APPIMAGE') or launcher_file).parent
    try:
        for config in configs:
            bbport_steam.add_shortcut(config / 'shortcuts.vdf', STEAM_NAME, exe, str(start_dir),
                                      str(icon) if icon.is_file() else '', options, match_options=True)
    except (OSError, ValueError) as error:
        launcher.messagebox.showerror(app_name, _('Could not add MediEvil to Steam: {}').format(error))
        return
    if restart:
        steam_start()
    launcher.messagebox.showinfo(app_name, _('MediEvil is in your Steam library. It starts with the settings '
                                             'saved in this launcher.'))


# ---- Steam Deck mode ------------------------------------------------------------------------
# One switch on the Display page that sets what suits the Deck's 1280x800 screen and its APU.
# Those settings are locked while it is on; turning it off restores the player's own values.
# On SteamOS it is on until the player turns it off.

# 30 by default: the Deck's CPU translates the PS4's GPU commands and does not hold 60 in the
# levels (upstream #16, #21); a steady 30 also gives longer battery life. 60 can be chosen.
DECK_FPS = [('30', ('30 (recommended)',)), ('60', ('60',))]
# medievil.ini values (the game and the in-game menu read the same file).
DECK_SETTINGS = {
    'display_mode': 'fullscreen',         # gamescope (Game Mode) scales the window to the screen
    'engine_fps': '60',                   # the Deck frame rate (30/60) is the limit below it
    'ue4_upscale': 'off',                 # the 720p scene is already the Deck's scale
    'fsr1': '0',
    # The 3D scene at 720p inside the game's 1080p output (gamescope scales that to the screen).
    # 720p_native (the whole output at 1280x720) showed the 1080p loading screens and movies
    # cropped, zoomed in with illegible text; at 30 FPS the Deck holds 720p the same.
    'render_resolution': '720p',
    'ue4_quality': 'low',                 # upstream's setting for integrated graphics
    'vsync': '1',
}
DECK_LOCKED = (*DECK_SETTINGS, 'fps_limit')
APP_DEFAULTS = {'deck_mode': False, 'deck_fps': '30', 'record_inputs': False, 'deck_steady_clock': True,
                'stereo_surround': 'off'}
# How the game's surround channels go into stereo (GOW3_AUDIO_SURROUND, runtime_audio.c): MediEvil's
# side and back channels carry delayed copies of positioned sounds, which doubled them in stereo.
STEREO_SURROUND = [('off', ('Off (front channels only)',)), ('reduced', ('Reduced (-9 dB)',)),
                   ('full', ('Full (-3 dB)',))]
STEREO_SURROUND_LEVELS = {'off': '0', 'reduced': '0.35', 'full': '0.71'}


def deck_updates(settings, ini_path):
    """Steam Deck mode's values changed: settings saved by an older launcher take them once
    (settings.json and, when the mode is on, medievil.ini), also for a start from Steam that never
    opens the launcher. 30 FPS became the default (deck_30fps) and the 720p scene scale replaced
    720p native output (deck_720p). Returns True when the settings changed (to be saved)."""
    import graphics_options
    changed = False
    if not settings.get('deck_30fps'):
        settings['deck_30fps'] = changed = True
        if settings.get('deck_fps', '30') != '30':
            settings['deck_fps'] = '30'
            if settings.get('deck_mode'):
                graphics_options.write_keys(ini_path, {'fps_limit': '30'})
    if not settings.get('deck_prompts'):
        # Steam Deck button pictures (button_prompts.py) for players in Steam Deck mode who kept
        # the PlayStation names.
        settings['deck_prompts'] = changed = True
        if settings.get('deck_mode') and settings.get('pad_style', 'playstation') == 'playstation':
            settings['pad_style'] = 'steamdeck'
    if not settings.get('deck_locks2'):
        # Engine frame rate, upscaling and FSR 1 joined the locked Steam Deck values.
        settings['deck_locks2'] = changed = True
        if settings.get('deck_mode'):
            graphics_options.write_keys(ini_path, {key: DECK_SETTINGS[key]
                                                   for key in ('engine_fps', 'ue4_upscale', 'fsr1')})
    if not settings.get('deck_720p'):
        settings['deck_720p'] = changed = True
        if settings.get('deck_mode') and \
                graphics_options.read_ini(ini_path).get('render_resolution') == '720p_native':
            graphics_options.write_keys(ini_path, {'render_resolution': '720p'})
    return changed


# ---- Options the launcher no longer shows -------------------------------------------------------
# Switching these could break the game (wrong colours or lighting, hangs, testing-only clocks), so
# the launcher leaves them out and puts them back to their defaults before every start: also when
# an older launcher or the in-game menu changed them. extra-env.txt still overrides them for tests.
HIDDEN_PAGES = ('patches',)  # its one built-in patch (the 4K framebuffer pool) must stay on
HIDDEN_APP = {'hdr': False, 'draw_pipe': '', 'readbacks': '', 'pipeline_binary_cache': True, 'perf_diag': False,
              'frame_capture_key': False, 'frame_stats': False, 'gpu_profile': False, 'vk_validation': False,
              'record_inputs': False, 'patches_dir': ''}
HIDDEN_INI = {'stale_readback': '0', 'deferred_readback': '1'}


def hidden_defaults(settings, ini_path):
    """Puts the hidden options back to their defaults (settings.json and medievil.ini).
    Returns True when settings.json changed (to be saved)."""
    import graphics_options
    changed = False
    for key, value in HIDDEN_APP.items():
        if key in settings and settings[key] != value:
            settings[key] = value
            changed = True
    if 'game_patches' in settings:  # the game's own patch list (defaults: on)
        del settings['game_patches']
        changed = True
    ini = graphics_options.read_ini(ini_path)
    stale = {key: value for key, value in HIDDEN_INI.items() if key in ini and ini[key] != value}
    if stale:
        graphics_options.write_keys(ini_path, stale)
    return changed


def steam_os():
    try:
        return any(line.strip() in ('ID=steamos', 'ID="steamos"')
                   for line in Path('/etc/os-release').read_text().splitlines())
    except OSError:
        return False


def deck_values(launcher):
    return {**DECK_SETTINGS, 'fps_limit': launcher.var('deck_fps', 'app').get()}


def set_var(launcher, key, value, ini_flags):
    var = launcher.var(key, 'ini')
    var.set(value == '1' if key in ini_flags else value)


def get_var(launcher, key, ini_flags):
    value = launcher.var(key, 'ini').get()
    return ('1' if value else '0') if key in ini_flags else str(value)


def locked_looks(launcher, theme):
    """Check box images for a locked setting (dimmed), as ttk image element states."""
    try:
        from PIL import Image
        n = max(14, int(16 * launcher.dpi))
        dim = []
        for checked in (True, False):
            image = theme.check_box(n, checked)
            image.putalpha(image.getchannel('A').point(lambda a: a * 2 // 5))
            dim.append(launcher.photo(image))
        return ('disabled selected', dim[0]), ('disabled', dim[1])
    except ImportError:
        return ()


def set_enabled(widget, enabled):
    """A setting's control and the title of its row (the cell to its left), greyed out or not."""
    try:
        widget.configure(state=('readonly' if widget.winfo_class() == 'TCombobox' else 'normal') if enabled
                         else 'disabled')
        info = widget.grid_info()
        if not info or int(info['column']) < 1:
            return
        for cell in widget.master.grid_slaves(row=int(info['row']), column=int(info['column']) - 1):
            for label in (cell, *cell.winfo_children()):
                if label.winfo_class() == 'TLabel':
                    label.state(['!disabled'] if enabled else ['disabled'])
    except Exception:
        pass


def deck_lock(launcher, on):
    for key in DECK_LOCKED:
        for widget in getattr(launcher, 'deck_widgets', {}).get(key, []):
            set_enabled(widget, not on)
    for widget in getattr(launcher, 'deck_fps_widgets', []):
        set_enabled(widget, on)


def deck_switch(launcher, ini_flags):
    """The checkbox changed: apply the Deck values (keeping the player's), or put those back."""
    on = launcher.var('deck_mode', 'app').get()
    if on:
        # The Deck's button pictures, unless the player chose Xbox ones.
        if launcher.app.get('pad_style', 'playstation') == 'playstation':
            launcher.app['pad_style'] = 'steamdeck'
            if hasattr(launcher, 'pad_style'):
                launcher.pad_style.set('steamdeck')
                launcher.relabel_pad_controls()
        launcher.app['deck_saved'] = {key: get_var(launcher, key, ini_flags) for key in DECK_LOCKED}
        for key, value in deck_values(launcher).items():
            set_var(launcher, key, value, ini_flags)
    else:
        for key, value in (launcher.app.pop('deck_saved', None) or {}).items():
            set_var(launcher, key, value, ini_flags)
    deck_lock(launcher, on)


def track_widget(launcher, key, widget):
    """choice()/check() call this (Linux fork) so the Deck settings can be locked."""
    if key in DECK_LOCKED:
        launcher.__dict__.setdefault('deck_widgets', {}).setdefault(key, []).append(widget)


def build_deck_section(launcher, frame, _, ini_flags):
    """The Steam Deck section at the top of the Display page."""
    launcher.section(frame, _('Steam Deck'), top=4)
    # Fullscreen, 720p render resolution, Low graphics quality and the frame rate below; those
    # settings are locked while it is on.
    launcher.check(frame, 'deck_mode', 'app', _('Steam Deck mode'))
    box = launcher.choice(frame, 'deck_fps', 'app', DECK_FPS, width=24)
    launcher.row(frame, _('Steam Deck frame rate'), box)
    # The game moves exactly one frame step per frame at the frame rate above (smooth pans and fades
    # after a slow frame); Steam Deck mode only.
    clock = launcher.check(frame, 'deck_steady_clock', 'app', _('Steady game clock'))
    launcher.section(frame, _('Sound'))
    # The game also plays delayed copies of its sound effects on the surround channels; in stereo
    # they sound doubled. Off leaves them out (front channels only).
    launcher.row(frame, _('Surround sounds in stereo'), launcher.choice(frame, 'stereo_surround', 'app', STEREO_SURROUND,
                                                                       width=24))
    launcher.deck_fps_widgets = [box, clock]
    launcher.var('deck_mode', 'app').trace_add('write', lambda *_a: deck_switch(launcher, ini_flags))
    launcher.var('deck_fps', 'app').trace_add('write', lambda *_a: launcher.var('deck_mode', 'app').get() and
                                             set_var(launcher, 'fps_limit', launcher.var('deck_fps', 'app').get(),
                                                     ini_flags))


def deck_start(launcher, ini_flags):
    """After the window is built: lock the Deck settings when the mode is on, and turn it on the
    first time the launcher opens on SteamOS."""
    if 'deck_mode' not in launcher.loaded_app and steam_os():
        launcher.var('deck_mode', 'app').set(True)  # deck_switch applies it
    else:
        deck_lock(launcher, launcher.var('deck_mode', 'app').get())


# ---- Sidebar ----------------------------------------------------------------------------------

def icon_image(name, size, color, background):
    """launcher/icons/<name>.png (white on transparent: Simple Icons, CC0) in `color` on
    `background`, as a PIL image of size x size."""
    from PIL import Image
    mask = Image.open(Path(__file__).resolve().parent / 'icons' / f'{name}.png').convert('RGBA') \
        .getchannel('A').resize((size, size), Image.LANCZOS)

    def rgb(value):
        return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))
    image = Image.new('RGB', (size, size), rgb(background))
    image.paste(Image.new('RGB', (size, size), rgb(color)), (0, 0), mask)
    return image


def fork_line(launcher, side, bg, gold, text):
    """'Linux fork by spacepepperoni' with Tux, under 'PC edition'."""
    tk = launcher.tk
    row = tk.Frame(side, bg=bg)
    row.pack(anchor='center', pady=(0, 20))
    try:
        tux = launcher.photo(icon_image('linux', launcher.px(15), gold, bg))
        tk.Label(row, image=tux, bg=bg, bd=0).pack(side='left', padx=(0, launcher.px(5)))
    except (ImportError, OSError):
        pass
    tk.Label(row, text='Linux fork by spacepepperoni', bg=bg, fg=text, font=launcher.f('body', 9), bd=0).pack(
        side='left')


# ---- Sidebar corner: GitHub, updates, version (as in the Uncharted Linux fork) ------------------
REPO_URL = f'https://github.com/{REPO}'


def download_icon(size, color, background, glow=0.0, glow_color=None):
    """A download symbol (an arrow into a tray) as a PIL image; `glow` 0..1 adds a soft halo."""
    from PIL import Image, ImageDraw, ImageFilter

    def rgb(value):
        return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))
    n = size * 4
    image = Image.new('RGB', (n, n), rgb(background))
    if glow > 0:
        halo = Image.new('L', (n, n), 0)
        ImageDraw.Draw(halo).ellipse((n * 0.22, n * 0.22, n * 0.78, n * 0.78), fill=int(170 * glow))
        halo = halo.filter(ImageFilter.GaussianBlur(n * 0.09))
        image.paste(Image.new('RGB', (n, n), rgb(glow_color or color)), (0, 0), halo)
    draw = ImageDraw.Draw(image)
    w = max(2, int(n * 0.11))
    c = rgb(color)
    draw.line((n * 0.5, n * 0.12, n * 0.5, n * 0.62), fill=c, width=w)            # shaft
    draw.polygon([(n * 0.27, n * 0.44), (n * 0.73, n * 0.44), (n * 0.5, n * 0.7)], fill=c)  # head
    draw.line((n * 0.14, n * 0.66, n * 0.14, n * 0.86), fill=c, width=w)          # tray
    draw.line((n * 0.14, n * 0.86, n * 0.86, n * 0.86), fill=c, width=w)
    draw.line((n * 0.86, n * 0.86, n * 0.86, n * 0.66), fill=c, width=w)
    return image.resize((size, size), Image.LANCZOS)


class Corner:
    """Bottom of the sidebar: the GitHub mark, the download button (grey: up to date; glowing and
    pulsing: a new version, click to install it) and the version."""

    def __init__(self, launcher, side, bg, text, muted, gold, version, _):
        import webbrowser
        tk = launcher.tk
        self.launcher, self.muted, self.gold, self.version, self._ = launcher, muted, gold, version, _
        self.state, self.new_version, self.url, self.page = 'none', None, None, RELEASES_PAGE
        self.glowing, self.frame = False, 0
        size = launcher.px(26)
        row = tk.Frame(side, bg=bg)
        row.pack(anchor='w', fill='x', padx=18, pady=(0, 18))
        photo = launcher.photo
        github_idle, github_hot = (photo(icon_image('github', size, c, bg)) for c in (muted, text))
        github = tk.Label(row, bg=bg, bd=0, image=github_idle, cursor='hand2')
        github.pack(side='left')
        github.bind('<Button-1>', lambda _e: webbrowser.open(REPO_URL))
        github.bind('<Enter>', lambda _e: github.configure(image=github_hot))
        github.bind('<Leave>', lambda _e: github.configure(image=github_idle))
        self.idle, self.hot = (photo(download_icon(size, c, bg)) for c in (muted, text))
        self.glow_images = [photo(download_icon(size, '#ffd877', bg, glow=g, glow_color=gold))
                            for g in (0.2, 0.4, 0.6, 0.8, 1.0, 0.8, 0.6, 0.4)]
        self.button = tk.Label(row, bg=bg, bd=0, image=self.idle, cursor='hand2')
        self.button.pack(side='left', padx=(launcher.px(12), 0))
        self.button.bind('<Button-1>', lambda _e: self.clicked())
        self.button.bind('<Enter>', lambda _e: self.state == 'none' and self.button.configure(image=self.hot))
        self.button.bind('<Leave>', lambda _e: self.state == 'none' and self.button.configure(image=self.idle))
        self.label = tk.Label(row, text=version, bg=bg, fg=muted, font=launcher.f('body', 9), bd=0)
        self.label.pack(side='left', padx=(launcher.px(8), 0))

    def show(self, text=None, fg=None):
        self.label.configure(text=text or self.version, fg=fg or self.muted)

    def set_state(self, state):
        self.state = state
        if state in ('available', 'downloading'):
            if not self.glowing:
                self.glowing = True
                self.glow()
        else:
            self.glowing = False
            self.button.configure(image=self.idle)

    def glow(self):
        if not self.glowing:
            return
        self.frame = (self.frame + 1) % len(self.glow_images)
        self.button.configure(image=self.glow_images[self.frame])
        self.launcher.root.after(110, self.glow)

    def offer(self, version, url, page):
        """A newer release (at launch or while open): the download button glows."""
        if self.state == 'downloading':
            return
        self.new_version, self.url, self.page = version, url, page or RELEASES_PAGE
        self.set_state('available')
        self.show(self._('{} available').format(version), self.gold)

    def clicked(self):
        launcher = self.launcher
        if self.state == 'available':
            self.install()
        elif self.state == 'none':
            self.show(self._('Checking…'))

            def check():
                launcher.check_update(manual=True)
                launcher.ui_calls.put(lambda: self.state == 'none' and self.show())
            threading.Thread(target=check, daemon=True).start()

    def install(self):
        """Downloads the new AppImage, puts it in its place and opens it (other installs: the
        release page)."""
        import webbrowser
        launcher, _ = self.launcher, self._
        if launcher.process:
            launcher.messagebox.showinfo(STEAM_NAME, _('Close the game before updating.'))
            return
        target = appimage()
        if not target or not self.url:
            webbrowser.open(self.page)
            return
        version = self.new_version
        self.set_state('downloading')

        def progress(percent):
            launcher.ui_calls.put(lambda: self.show(_('Downloading {}%').format(percent), self.gold))

        def restart():
            subprocess.Popen([str(target)], env=clean_env(), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
            launcher.root.destroy()

        def work():
            try:
                install_appimage(self.url, target, progress)
                launcher.ui_calls.put(restart)
            except (OSError, ValueError) as failure:
                def failed(error=failure):
                    self.set_state('available')
                    self.show(_('{} available').format(version), self.gold)
                    launcher.messagebox.showerror(STEAM_NAME, _('Update failed: {}').format(error))
                launcher.ui_calls.put(failed)
        threading.Thread(target=work, daemon=True).start()


def periodic_update_check(launcher):
    """At launch and every 30 minutes while the launcher is open."""
    if launcher.app.get('update_check', True) and launcher.corner.state == 'none':
        threading.Thread(target=launcher.check_update, daemon=True).start()
    launcher.root.after(30 * 60 * 1000, lambda: periodic_update_check(launcher))
