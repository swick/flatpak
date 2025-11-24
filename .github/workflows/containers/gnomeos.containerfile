FROM quay.io/gnome_infrastructure/gnome-build-meta:sdk-nightly

RUN git clone https://github.com/polkit-org/polkit.git
RUN meson setup _build --prefix=/usr polkit --libdir="lib/$(uname -m)-linux-gnu"
RUN meson install -C _build
RUN rm -rf _build polkit

RUN git clone https://github.com/libfuse/libfuse.git
RUN meson setup _build --prefix=/usr libfuse --libdir="lib/$(uname -m)-linux-gnu"
RUN meson install -C _build
RUN rm -rf _build libfuse