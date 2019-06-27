// Copyright 2019 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
package com.google.devtools.build.lib.remote.merkletree;

import build.bazel.remote.execution.v2.Digest;
import build.bazel.remote.execution.v2.Directory;
import build.bazel.remote.execution.v2.DirectoryNode;
import build.bazel.remote.execution.v2.FileNode;
import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableMap;
import com.google.common.collect.Iterables;
import com.google.common.collect.Maps;
import com.google.devtools.build.lib.actions.ActionInput;
import com.google.devtools.build.lib.actions.FileArtifactValue;
import com.google.devtools.build.lib.actions.MetadataProvider;
import com.google.devtools.build.lib.actions.cache.VirtualActionInput;
import com.google.devtools.build.lib.profiler.Profiler;
import com.google.devtools.build.lib.profiler.SilentCloseable;
import com.google.devtools.build.lib.remote.util.DigestUtil;
import com.google.devtools.build.lib.vfs.Path;
import com.google.devtools.build.lib.vfs.PathFragment;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.SortedMap;
import javax.annotation.Nullable;

/** A merkle tree representation as defined by the remote execution api. */
public final class MerkleTree {

  private final Map<Digest, Directory> digestDirectoryMap;
  private final Map<Digest, ActionInput> digestActionInputMap;
  private final Digest rootDigest;

  private MerkleTree(
      Map<Digest, Directory> digestDirectoryMap,
      Map<Digest, ActionInput> digestActionInputMap,
      Digest rootDigest) {
    this.digestDirectoryMap = digestDirectoryMap;
    this.digestActionInputMap = digestActionInputMap;
    this.rootDigest = rootDigest;
  }

  /** Returns the digest of the merkle tree's root. */
  public Digest getRootDigest() {
    return rootDigest;
  }

  @Nullable
  public Directory getDirectoryByDigest(Digest digest) {
    return digestDirectoryMap.get(digest);
  }

  @Nullable
  public ActionInput getInputByDigest(Digest digest) {
    return digestActionInputMap.get(digest);
  }

  /**
   * Returns the hashes of all nodes and leafs of the merkle tree. That is, the hashes of the {@link
   * Directory} protobufs and {@link ActionInput} files.
   */
  public Iterable<Digest> getAllDigests() {
    return Iterables.concat(digestDirectoryMap.keySet(), digestActionInputMap.keySet());
  }

  public static Digest computeRootDigest(
      SortedMap<PathFragment, ActionInput> inputs,
      MetadataProvider metadataProvider,
      Path execRoot,
      DigestUtil digestUtil)
      throws IOException {
    return build(inputs, metadataProvider, execRoot, digestUtil, null, null);
  }

  private static Digest build(
      SortedMap<PathFragment, ActionInput> inputs,
      MetadataProvider metadataProvider,
      Path execRoot,
      DigestUtil digestUtil,
      @Nullable Map<Digest, Directory> digestDirectoryMap,
      @Nullable Map<Digest, ActionInput> digestActionInputMap)
      throws IOException {
    PathFragment currentDirectory = PathFragment.EMPTY_FRAGMENT;
    ArrayList<Directory.Builder> directoryStack = new ArrayList<>();
    Directory.Builder currentDirectoryBuilder = Directory.newBuilder();
    directoryStack.add(currentDirectoryBuilder);
    for (Map.Entry<PathFragment, ActionInput> e : inputs.entrySet()) {
      PathFragment path = e.getKey();
      ActionInput input = e.getValue();
      PathFragment pathDirectory = path.getParentDirectory();
      if (!pathDirectory.equals(currentDirectory)) {
        while (!pathDirectory.startsWith(currentDirectory)) {
          Directory finishedDirectory = directoryStack.remove(directoryStack.size() - 1).build();
          Digest directoryDigest = digestUtil.compute(finishedDirectory);
          if (digestDirectoryMap != null) {
            digestDirectoryMap.put(directoryDigest, finishedDirectory);
          }
          directoryStack
              .get(directoryStack.size() - 1)
              .addDirectoriesBuilder()
              .setName(currentDirectory.getBaseName())
              .setDigest(directoryDigest);
          currentDirectory = currentDirectory.getParentDirectory();
        }
        int newSegments = pathDirectory.relativeTo(currentDirectory).segmentCount();
        for (int i = 0; i < newSegments; i++) {
          directoryStack.add(Directory.newBuilder());
        }
        currentDirectoryBuilder = directoryStack.get(directoryStack.size() - 1);
        currentDirectory = pathDirectory;
      }
      Digest digest;
      if (input instanceof VirtualActionInput) {
        digest = digestUtil.compute((VirtualActionInput) input);
      } else {
        FileArtifactValue metadata =
            Preconditions.checkNotNull(
                metadataProvider.getMetadata(input),
                "missing metadata for '%s'",
                input.getExecPathString());
        Preconditions.checkState(metadata.getType() == com.google.devtools.build.lib.actions.FileStateType.REGULAR_FILE);
        digest = digestUtil.buildDigest(metadata.getDigest(), metadata.getSize());
      }
      if (digestActionInputMap != null) {
        digestActionInputMap.put(digest, input);
      }
      currentDirectoryBuilder
          .addFilesBuilder()
          .setName(path.getBaseName())
          .setDigest(digest)
          .setIsExecutable(true);
    }
    Digest directoryDigest;
    while (true) {
      Directory finishedDirectory = directoryStack.remove(directoryStack.size() - 1).build();
      directoryDigest = digestUtil.compute(finishedDirectory);
      if (digestDirectoryMap != null) {
        digestDirectoryMap.put(directoryDigest, finishedDirectory);
      }
      if (directoryStack.isEmpty()) {
        break;
      }
      directoryStack
          .get(directoryStack.size() - 1)
          .addDirectoriesBuilder()
          .setName(currentDirectory.getBaseName())
          .setDigest(directoryDigest);
      currentDirectory = currentDirectory.getParentDirectory();
    }
    Preconditions.checkState(currentDirectory.equals(PathFragment.EMPTY_FRAGMENT));
    return directoryDigest;
  }

  /**
   * Constructs a merkle tree from a lexicographically sorted map of inputs (files).
   *
   * @param inputs a map of path to input. The map is required to be sorted lexicographically by
   *     paths. Inputs of type tree artifacts are not supported and are expected to have been
   *     expanded before.
   * @param metadataProvider provides metadata for all {@link ActionInput}s in {@code inputs}, as
   *     well as any {@link ActionInput}s being discovered via directory expansion.
   * @param execRoot all paths in {@code inputs} need to be relative to this {@code execRoot}.
   * @param digestUtil a hashing utility
   */
  public static MerkleTree build(
      SortedMap<PathFragment, ActionInput> inputs,
      MetadataProvider metadataProvider,
      Path execRoot,
      DigestUtil digestUtil)
      throws IOException {
    try (SilentCloseable c = Profiler.instance().profile("MerkleTree.build")) {
      Map<Digest, Directory> digestDirectoryMap = Maps.newHashMap();
      Map<Digest, ActionInput> digestActionInputMap =
          Maps.newHashMapWithExpectedSize(inputs.size());
      Digest root =
          build(
              inputs,
              metadataProvider,
              execRoot,
              digestUtil,
              digestDirectoryMap,
              digestActionInputMap);
      return new MerkleTree(digestDirectoryMap, digestActionInputMap, root);
    }
  }

  private static MerkleTree build(InputTree tree, DigestUtil digestUtil) {
    Preconditions.checkNotNull(tree);
    if (tree.isEmpty()) {
      return new MerkleTree(ImmutableMap.of(), ImmutableMap.of(), digestUtil.compute(new byte[0]));
    }
    Map<Digest, Directory> digestDirectoryMap =
        Maps.newHashMapWithExpectedSize(tree.numDirectories());
    Map<Digest, ActionInput> digestActionInputMap =
        Maps.newHashMapWithExpectedSize(tree.numFiles());
    Map<PathFragment, Digest> m = new HashMap<>();
    tree.visit(
        (dirname, files, dirs) -> {
          Directory.Builder b = Directory.newBuilder();
          for (InputTree.FileNode file : files) {
            b.addFiles(buildProto(file));
            digestActionInputMap.put(file.getDigest(), file.getActionInput());
          }
          for (InputTree.DirectoryNode dir : dirs) {
            PathFragment subDirname = dirname.getRelative(dir.getPathSegment());
            Digest protoDirDigest =
                Preconditions.checkNotNull(m.remove(subDirname), "protoDirDigest was null");
            b.addDirectories(buildProto(dir, protoDirDigest));
          }
          Directory protoDir = b.build();
          Digest protoDirDigest = digestUtil.compute(protoDir);
          digestDirectoryMap.put(protoDirDigest, protoDir);
          m.put(dirname, protoDirDigest);
        });
    return new MerkleTree(
        digestDirectoryMap, digestActionInputMap, m.get(PathFragment.EMPTY_FRAGMENT));
  }

  private static FileNode buildProto(InputTree.FileNode file) {
    return FileNode.newBuilder()
        .setName(file.getPathSegment())
        .setDigest(file.getDigest())
        .setIsExecutable(true)
        .build();
  }

  private static DirectoryNode buildProto(InputTree.DirectoryNode dir, Digest protoDirDigest) {
    return DirectoryNode.newBuilder()
        .setName(dir.getPathSegment())
        .setDigest(protoDirDigest)
        .build();
  }
}
