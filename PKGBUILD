pkgname=sinwm
pkgver=1.1.2
pkgrel=1
pkgdesc="Sin Window Manager"
arch=('x86_64')
url="https://github.com/porsager/sinwm"
depends=('libxcb' 'xcb-util-wm' 'xcb-util-image' 'libpng')
source=("sinwm.c")
md5sums=('SKIP')

build() {
  gcc -o "$pkgname" "$srcdir/$pkgname.c" -lxcb -lxcb-xinput -lxcb-icccm -lxcb-randr -lxcb-image -lpng
}

package() {
  install -Dm755 "$pkgname" "$pkgdir/usr/bin/$pkgname"
}
