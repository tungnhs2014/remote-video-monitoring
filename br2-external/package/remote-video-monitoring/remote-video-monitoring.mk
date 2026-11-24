################################################################################
#
# remote-video-monitoring
#
################################################################################

# Development source only: includes current working-tree changes. A release
# must replace this with an approved immutable revision or source archive.
REMOTE_VIDEO_MONITORING_SITE = $(BR2_EXTERNAL_RVM_PATH)/..
REMOTE_VIDEO_MONITORING_SITE_METHOD = local
# Buildroot legal-info archives local sources without the exclusions below.
REMOTE_VIDEO_MONITORING_REDISTRIBUTE = NO
REMOTE_VIDEO_MONITORING_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = \
	--exclude /.agents/ \
	--exclude /.codex/ \
	--exclude /.local-artifacts/ \
	--exclude /.local-reference/ \
	--exclude /br2-external/ \
	--exclude /docs/ \
	--exclude tests/ \
	--exclude /build/ \
	--exclude '/build-*/' \
	--exclude '/cmake-build-*/' \
	--exclude /out/ \
	--exclude /AGENTS.md \
	--exclude /SOURCE_EVIDENCE.md

REMOTE_VIDEO_MONITORING_DEPENDENCIES = \
	grpc protobuf host-grpc host-protobuf \
	gstreamer1 gst1-plugins-base gst1-plugins-good

REMOTE_VIDEO_MONITORING_SUPPORTS_IN_SOURCE_BUILD = NO
REMOTE_VIDEO_MONITORING_CONF_OPTS = \
	-DBUILD_TESTING=OFF \
	-DCAMSTREAM_ENABLE_GRPC=ON \
	-DCAMSTREAM_M1_GSTREAMER_SMOKE=OFF \
	-DCAMSTREAM_M4_SESSION_SMOKE=OFF \
	-DCAMSTREAM_PROTOC_EXECUTABLE=$(HOST_DIR)/bin/protoc \
	-DCAMSTREAM_GRPC_CPP_PLUGIN_EXECUTABLE=$(HOST_DIR)/bin/grpc_cpp_plugin \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DCMAKE_SKIP_INSTALL_RPATH=ON

# CMake's ordinary install still includes development and diagnostic targets.
# The Buildroot image installs only the four production runtime targets.
REMOTE_VIDEO_MONITORING_INSTALL_TARGET_OPTS = --component Runtime

define REMOTE_VIDEO_MONITORING_INSTALL_CONFIG
	$(INSTALL) -D -m 0644 $(REMOTE_VIDEO_MONITORING_PKGDIR)/remote-video-monitoring.conf \
		$(TARGET_DIR)/etc/default/remote-video-monitoring
	ln -sf gstreamer-1.0/libgstcamstreamsrc.so $(TARGET_DIR)/usr/lib/libgstcamstreamsrc.so
endef
REMOTE_VIDEO_MONITORING_POST_INSTALL_TARGET_HOOKS += REMOTE_VIDEO_MONITORING_INSTALL_CONFIG

define REMOTE_VIDEO_MONITORING_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(REMOTE_VIDEO_MONITORING_PKGDIR)/S90remote-video-monitoring \
		$(TARGET_DIR)/etc/init.d/S90remote-video-monitoring
endef

$(eval $(cmake-package))
