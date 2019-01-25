package com.google.devtools.build.lib.sandbox;

import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.Maps;

import com.google.devtools.build.lib.cmdline.Label;
import com.google.devtools.build.lib.events.Event;
import com.google.devtools.build.lib.events.Reporter;
import com.google.devtools.build.lib.packages.FileTarget;
import com.google.devtools.build.lib.packages.NoSuchThingException;
import com.google.devtools.build.lib.packages.RawAttributeMapper;
import com.google.devtools.build.lib.packages.Rule;
import com.google.devtools.build.lib.packages.Target;
import com.google.devtools.build.lib.runtime.CommandEnvironment;
import com.google.devtools.build.lib.unix.NativePosixFiles;
import com.google.devtools.build.lib.vfs.FileStatus;
import com.google.devtools.build.lib.vfs.FileSystem;
import com.google.devtools.build.lib.vfs.FileSystemUtils;
import com.google.devtools.build.lib.vfs.Path;
import com.google.devtools.build.lib.vfs.PathFragment;
import com.google.devtools.build.lib.vfs.Symlinks;

import org.apache.commons.codec.digest.DigestUtils;
import org.apache.commons.compress.archivers.tar.TarArchiveEntry;
import org.apache.commons.compress.archivers.tar.TarArchiveInputStream;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.IOException;
import java.net.URL;
import java.nio.file.Files;
import java.nio.file.InvalidPathException;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.zip.GZIPInputStream;


/**
 * Helper class to help manage custom rootfs images for the sandbox.
 */
public final class LinuxSandboxRootfsManager {
  private final FileSystem fs;
  private final String rootfsBase;
  private final Label rootfsLabel;

  public static final Set<String> MOUNT_BLACKLIST = new HashSet<String>(
    Arrays.asList(new String[]{
    "dev",
    "home",
    "mnt",
    "proc",
    "root",
    "srv",
    "sys",
    "tmp",
  }));

  private static final Set<String> RW_MOUNTS = new HashSet<String>(
    Arrays.asList(new String[]{"/etc", "/run", "/srv"})
  );

  private static final Set<String> MOUNTS_FROM_HOST = new HashSet<String>(
    Arrays.asList(new String[]{"/etc/hosts", "/etc/resolv.conf"})
  );

  private LinuxSandboxRootfsManager(FileSystem fs, String rootfsBase, Label rootfsLabel) {
    this.fs = fs;
    this.rootfsBase = rootfsBase;
    this.rootfsLabel = rootfsLabel;
  }

  public Label getRootfsLabel() {
    return rootfsLabel;
  }

  public interface LazyInputStream {
    public InputStream getStream() throws IOException;
  }

  public static LinuxSandboxRootfsManager create(FileSystem fs, Label rootfsLabel, String rootfsCachePath, CommandEnvironment env) throws IOException {
    String rootfsBase = null;
    try {
      Target target = env.getPackageManager().getTarget(env.getReporter(), rootfsLabel);
      if (target instanceof Rule) {
        Rule rule = (Rule) target;
        if (!rule.getRuleClass().equals("filegroup")) {
          throw new IllegalArgumentException("sandbox_rootfs must either be a filegroup or a file target.");
        }
        ImmutableList attr = (ImmutableList) RawAttributeMapper.of(rule).getRawAttributeValue(rule, "srcs");
        if (attr.size() != 1) {
          throw new IllegalArgumentException("sandbox_rootfs filegroup must have exactly one file.");
        }
        rootfsLabel = (Label) attr.get(0);
      } else if (!(target instanceof FileTarget)) {
        throw new IllegalArgumentException("sandbox_rootfs must either be a filegroup or a file target.");
      }
      Path rootfsArchivePath = env.getBlazeModuleEnvironment().getFileFromWorkspace(rootfsLabel);
      rootfsBase = getRootfsPath(env, rootfsArchivePath, rootfsCachePath);
    } catch (NoSuchThingException | InterruptedException e) {
      // TODO() better error handling
      throw new IllegalArgumentException(e);
    }
    return new LinuxSandboxRootfsManager(env.getRuntime().getFileSystem(), rootfsBase, rootfsLabel);
  }

  private static String getRootfsPath(CommandEnvironment env, Path archivePath, String rootfsCachePath) throws IOException {
    return getRootfsPath(env, archivePath.getPathString(), rootfsCachePath, ()-> new FileInputStream(archivePath.getPathString()));
  }

  private static String getRootfsPath(CommandEnvironment env, String rootfsName, String rootfsCachePath, LazyInputStream lazyStream) throws IOException {
    Path basePath = env.getRuntime().getFileSystem().getPath(rootfsCachePath).getRelative(DigestUtils.sha256Hex(rootfsName));
    String basePathString = basePath.getPathString();
    if (basePath.exists()) {
      return basePathString;
    }
    env.getReporter().handle(Event.info("Creating new rootfs image for " + rootfsName));
    InputStream stream = null;
    TarArchiveInputStream tarStream = null;
    try {
      stream = lazyStream.getStream();
      tarStream = new TarArchiveInputStream(new GZIPInputStream(stream));
      TarArchiveEntry tarEntry;
      while ((tarEntry = tarStream.getNextTarEntry()) != null) {
        extractTarEntry(basePath, tarStream, tarEntry);
      }

      return basePathString;
    } catch(Exception e) {
      if (basePath.exists()) {
        FileSystemUtils.deleteTree(basePath);
      }
      throw e;
    } finally {
      if (tarStream != null) {
        tarStream.close();
      }
      if (stream != null) {
        stream.close();
      }
    }
  }

  private static void extractTarEntry(Path basePath, TarArchiveInputStream tarStream, TarArchiveEntry tarEntry) throws IOException {
    String name = tarEntry.getName();
    for (String prefix: MOUNT_BLACKLIST) {
      if (name.startsWith(prefix)) {
        return;
      }
    }
    Path filename = basePath.getRelative(name);
    FileSystemUtils.createDirectoryAndParents(filename.getParentDirectory());
    if (tarEntry.isDirectory()) {
      FileSystemUtils.createDirectoryAndParents(filename);
    } else {
      if (tarEntry.isSymbolicLink()) {
        PathFragment linkName = PathFragment.create(tarEntry.getLinkName());
        try {
          FileSystemUtils.ensureSymbolicLink(filename, linkName);
        } catch (IOException e) {
          // TODO() this is most likely unicode file name issue, but the exception is way too broad
        }
      } else {
        try {
          Files.copy(
            tarStream, filename.getPathFile().toPath(), StandardCopyOption.REPLACE_EXISTING);
          filename.chmod(tarEntry.getMode());
        } catch (InvalidPathException e) {
          // TODO() this is most likely unicode file name issue
        }
      }
    }
  }


  public void addROMounts(Map<Path, Path> mounts) throws IOException {
    Map<Path, Path> rootfsMounts = Maps.newTreeMap();
    for (String entry : NativePosixFiles.readdir(rootfsBase)) {
      // do this check even though we already do the check on extraction,
      // in case the user hasn't run `bazel clean --expunge` in a while
      // and the list has changed
      if (MOUNT_BLACKLIST.contains(entry)) {
        continue;
      }
      Path libDir = fs.getPath("/").getRelative(entry);
      Path rootfsLibDir = fs.getPath("/").getRelative(rootfsBase + "/" + entry);
      rootfsMounts.put(libDir, rootfsLibDir);
    }
    for (Path path : rootfsMounts.values()) {
      Preconditions.checkArgument(path.exists(), "%s does not exist", path.toString());
    }
    for (Map.Entry<Path, Path> entry : handleRWMounts(rootfsMounts).entrySet()) {
      mounts.put(entry.getKey(), entry.getValue());
    }
    for (String entry: MOUNTS_FROM_HOST) {
      Path entryPath = fs.getPath(entry);
      if (entryPath.exists()) {
        mounts.put(entryPath, entryPath);
      }
    }
  }

  /**
   * For each directory in the mount map, if it is a RW mount, then mount
   * everything immediately inside the directory instead. This allows
   * the directory itself to be mounted as RW.
   *
   * @return a new mounts multimap with all mounts and the added mounts.
   */
  Map<Path, Path> handleRWMounts(Map<Path, Path> mounts) throws IOException {
    Map<Path, Path> finalizedMounts = Maps.newTreeMap();
    Path rootfsBasePath = fs.getPath("/");
    if (rootfsBase != null) {
      rootfsBasePath = fs.getPath(rootfsBase);
    }
    Path rootPath = fs.getPath("/");
    for (Map.Entry<Path, Path> mount : mounts.entrySet()) {
      Path target = mount.getKey();
      Path source = mount.getValue();

      FileStatus stat = source.statNullable(Symlinks.NOFOLLOW);

      if (stat != null && stat.isDirectory() && RW_MOUNTS.contains(target.toString())) {
        for (String entry : NativePosixFiles.readdir(source.toString())) {
          Path subSource = source.getRelative(entry);
          Path subTarget = target.getRelative(entry);
          FileStatus subStat = subSource.statNullable(Symlinks.NOFOLLOW);
          if (subStat != null && subStat.isSymbolicLink()) {
            // if this is a symlink, and it doesn't exist, we'll have trouble mounting it.
            // so, let's just skip it.
            PathFragment symlinkTargetFragment = subSource.readSymbolicLink();
            Path symlinkTargetPath;
            if (symlinkTargetFragment.isAbsolute()) {
              symlinkTargetPath = rootfsBasePath.getRelative(symlinkTargetFragment.relativeTo(rootPath.asFragment()));
            } else {
              symlinkTargetPath = target.getRelative(symlinkTargetFragment);
            }
            if (!symlinkTargetPath.exists()) {
              continue;
            }
          }
          finalizedMounts.put(subTarget, subSource);
        }
      } else {
        finalizedMounts.put(target, source);
      }
    }
    return finalizedMounts;
  }
}
