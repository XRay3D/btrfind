#!/usr/bin/env bash
# Сборка и установка btrfind (демон btrfindd + GUI btrfind) в систему.
#
#   ./install.sh                 собрать, прогнать тесты, установить и запустить сервис
#   ./install.sh build           только собрать (и протестировать)
#   ./install.sh install         установить уже собранное
#   ./install.sh uninstall       остановить сервис и удалить установленные файлы (--purge: и индексы)
#   ./install.sh status          состояние сервиса и томов
#
# Опции: --prefix DIR (/usr)  --build-dir DIR (build)  --debug  --no-service  --skip-tests  --purge
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

PREFIX=/usr
BUILD_DIR=build
BUILD_TYPE=Release
SERVICE=1
TESTS=1
PURGE=0
CMD=all

while [[ $# -gt 0 ]]; do
    case "$1" in
        build|install|uninstall|status|all) CMD=$1 ;;
        --prefix) PREFIX=$2; shift ;;
        --build-dir) BUILD_DIR=$2; shift ;;
        --debug) BUILD_TYPE=Debug ;;
        --no-service) SERVICE=0 ;;
        --skip-tests) TESTS=0 ;;
        --purge) PURGE=1 ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Неизвестный аргумент: $1 (см. --help)" >&2; exit 2 ;;
    esac
    shift
done

if [[ -t 1 ]]; then B=$'\e[1m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; N=$'\e[0m'; else B= G= Y= R= N=; fi
step() { echo "${B}==>${N} ${B}$*${N}"; }
ok()   { echo "  ${G}✓${N} $*"; }
warn() { echo "  ${Y}!${N} $*"; }
die()  { echo "${R}Ошибка:${N} $*" >&2; exit 1; }

SUDO=sudo
[[ $EUID -eq 0 ]] && SUDO=

check_deps() {
    step "Проверка зависимостей"
    local missing=()
    command -v cmake >/dev/null      || missing+=("cmake")
    command -v g++ >/dev/null        || missing+=("gcc-c++")
    command -v pkg-config >/dev/null || missing+=("pkgconf")
    if command -v pkg-config >/dev/null; then
        pkg-config --exists Qt6Core Qt6Network Qt6Widgets Qt6DBus Qt6Test || missing+=("qt6-base-devel")
        pkg-config --exists libpcre2-8 || missing+=("libpcre2-devel")
    fi
    [[ -f /usr/include/linux/btrfs.h ]] || missing+=("glibc-kernheaders")
    local kio=0
    for d in /usr/lib64/cmake/KF6KIO /usr/lib/cmake/KF6KIO /usr/lib/x86_64-linux-gnu/cmake/KF6KIO; do [[ -d $d ]] && kio=1; done
    if [[ ${#missing[@]} -gt 0 ]]; then
        die "не хватает пакетов: ${missing[*]}
  ALT Linux:  sudo apt-get install ${missing[*]} libgomp-devel"
    fi
    ok "cmake $(cmake --version | head -1 | awk '{print $3}'), g++ $(g++ -dumpversion), Qt $(pkg-config --modversion Qt6Core), PCRE2 $(pkg-config --modversion libpcre2-8)"
    if [[ $kio -eq 1 ]]; then
        ok "эскизы: KDE KIO (видео, PDF, документы — как в Dolphin)"
    else
        warn "эскизы только для изображений: для видео/PDF установите kf6-kio-devel и пересоберите"
    fi

    local kmaj kmin
    IFS=. read -r kmaj kmin _ <<<"$(uname -r)"
    if (( kmaj < 5 || (kmaj == 5 && kmin < 17) )); then
        warn "ядро $(uname -r): для живых обновлений нужно ≥ 5.17 (FAN_RENAME), для подтомов — ≥ 6.8"
    fi
    if ! findmnt -t btrfs -n >/dev/null 2>&1; then
        warn "смонтированных btrfs не найдено — индексировать будет нечего"
    fi
}

do_build() {
    check_deps
    step "Сборка ($BUILD_TYPE) в $BUILD_DIR"
    cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
          -DBTRFIND_BUILD_TESTS="$([[ $TESTS -eq 1 ]] && echo ON || echo OFF)" >/dev/null
    cmake --build "$BUILD_DIR" -j"$(nproc)"
    ok "собрано: $BUILD_DIR/btrfindd, $BUILD_DIR/btrfind"
    if [[ $TESTS -eq 1 ]]; then
        step "Тесты"
        ctest --test-dir "$BUILD_DIR" --output-on-failure
    fi
}

wait_socket() {
    local sock=/run/btrfind/btrfind.sock
    for _ in $(seq 1 30); do
        [[ -S $sock ]] && return 0
        sleep 1
    done
    return 1
}

# Пункт «Искать здесь (btrfind)…» в контекстном меню папок Dolphin (как «Search Everything…» в Проводнике)
# и ярлык в меню приложений. Файлы ставит cmake --install; здесь — проверка и обновление кэша KDE.
install_menus() {
    step "Меню: приложение и пункт в контекстном меню Dolphin"
    local sm="$PREFIX/share/kio/servicemenus/btrfind-servicemenu.desktop"
    local app="$PREFIX/share/applications/btrfind.desktop"
    if [[ ! -f $sm ]]; then  # например, сборка без GUI-части — ставим пункт напрямую
        $SUDO install -Dm644 packaging/btrfind-servicemenu.desktop "$sm"
    fi
    [[ -f $sm ]] && ok "контекстное меню папок: $sm" || warn "не удалось установить $sm"
    [[ -f $app ]] && ok "меню приложений: $app"

    # Кэш сервисов KDE обновляется от имени пользователя сессии, а не root.
    local user=${SUDO_USER:-$USER}
    local run=()
    [[ $EUID -eq 0 && -n ${SUDO_USER:-} ]] && run=(sudo -u "$SUDO_USER" --preserve-env=DISPLAY,WAYLAND_DISPLAY,XDG_RUNTIME_DIR,DBUS_SESSION_BUS_ADDRESS)
    local tool
    for tool in kbuildsycoca6 kbuildsycoca5; do
        if command -v "$tool" >/dev/null; then
            "${run[@]}" "$tool" >/dev/null 2>&1 && ok "кэш меню KDE обновлён ($tool, пользователь $user)" \
                || warn "$tool не сработал — пункт появится после повторного входа в сеанс"
            break
        fi
    done
    command -v update-desktop-database >/dev/null && $SUDO update-desktop-database -q "$PREFIX/share/applications" || true
}

do_install() {
    [[ -f "$BUILD_DIR/btrfindd" && -f "$BUILD_DIR/btrfind" ]] || die "сначала соберите: ./install.sh build"
    step "Установка в $PREFIX (нужен sudo)"
    $SUDO cmake --install "$BUILD_DIR"
    ok "файлы установлены (список: $BUILD_DIR/install_manifest.txt)"

    install_menus

    [[ $SERVICE -eq 1 ]] || { warn "сервис не запускался (--no-service)"; return; }
    command -v systemctl >/dev/null || { warn "systemd не найден — запустите $PREFIX/sbin/btrfindd вручную от root"; return; }
    step "Сервис btrfindd"
    $SUDO systemctl daemon-reload
    if systemctl is-active --quiet btrfindd; then
        $SUDO systemctl restart btrfindd
        ok "перезапущен"
    else
        $SUDO systemctl enable --now btrfindd
        ok "включён и запущен"
    fi
    if ! wait_socket; then
        service_failed "сокет не появился за 30 с"
    fi
    ok "сокет /run/btrfind/btrfind.sock готов"
    # Дождаться конца первичной индексации (и убедиться, что сервис не упал по ходу).
    local st
    for _ in $(seq 1 120); do
        sleep 1
        systemctl is-active --quiet btrfindd || service_failed "сервис остановился во время индексации"
        st=$("$PREFIX/bin/btrfind" --status 2>/dev/null) || continue
        grep -qE 'pending|loading|scanning|catching-up' <<<"$st" || break
    done
    systemctl is-active --quiet btrfindd || service_failed "сервис остановился"
    "$PREFIX/bin/btrfind" --status || service_failed "демон не отвечает"
    grep -q 'error' <<<"$st" && warn "у некоторых томов ошибка — journalctl -u btrfindd -e"
    echo
    ok "готово. Запуск: ${B}btrfind${N} из меню приложений; консольный поиск: ${B}btrfind --cli запрос${N}"
}

service_failed() {
    echo "${R}Ошибка:${N} $1. Последние записи журнала:" >&2
    $SUDO journalctl -u btrfindd -n 25 --no-pager -o cat >&2 || true
    exit 1
}

do_uninstall() {
    step "Удаление"
    if command -v systemctl >/dev/null && systemctl list-unit-files btrfindd.service >/dev/null 2>&1; then
        $SUDO systemctl disable --now btrfindd 2>/dev/null || true
        ok "сервис остановлен и отключён"
    fi
    local manifest="$BUILD_DIR/install_manifest.txt"
    if [[ -f $manifest ]]; then
        xargs -d '\n' -a "$manifest" $SUDO rm -f --
        ok "удалены файлы из $manifest"
    else
        local f
        for f in "$PREFIX/sbin/btrfindd" "$PREFIX/bin/btrfind" "$PREFIX/bin/btrfs-poc" \
                 "$PREFIX/lib/systemd/system/btrfindd.service" "$PREFIX/share/applications/btrfind.desktop" \
                 "$PREFIX/share/kio/servicemenus/btrfind-servicemenu.desktop"; do
            $SUDO rm -f -- "$f"
        done
        $SUDO rm -rf -- "$PREFIX/share/doc/btrfind"
        ok "удалены стандартные пути (manifest не найден)"
    fi
    command -v systemctl >/dev/null && $SUDO systemctl daemon-reload || true
    if command -v kbuildsycoca6 >/dev/null; then  # убрать пункт из меню Dolphin
        if [[ $EUID -eq 0 && -n ${SUDO_USER:-} ]]; then sudo -u "$SUDO_USER" kbuildsycoca6 >/dev/null 2>&1 || true
        else kbuildsycoca6 >/dev/null 2>&1 || true; fi
    fi
    if [[ $PURGE -eq 1 ]]; then
        $SUDO rm -rf -- /var/lib/btrfind
        ok "удалены сохранённые индексы /var/lib/btrfind"
    else
        echo "  сохранённые индексы оставлены в /var/lib/btrfind (удалить: ./install.sh uninstall --purge)"
    fi
}

do_status() {
    systemctl status btrfindd --no-pager -n 15 || true
    echo
    command -v btrfind >/dev/null && btrfind --status || true
}

case $CMD in
    build) do_build ;;
    install) do_install ;;
    uninstall) do_uninstall ;;
    status) do_status ;;
    all) do_build; do_install ;;
esac
