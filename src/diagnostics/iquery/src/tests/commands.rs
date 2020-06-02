// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use {
    crate::{assert_command, commands::expand_paths, tests::utils, types::Error},
    fuchsia_async as fasync,
    matches::assert_matches,
    std::path::Path,
    tempfile::tempdir,
};

// List command

#[fasync::run_singlethreaded(test)]
async fn test_list() {
    let (_env, _app) = utils::start_basic_component("list-test").await.expect("create comp 1");
    let (_env2, _app2) = utils::start_basic_component("list-test2").await.expect("create comp 2");
    assert_command!(
        command: "list",
        golden_basename: list_test,
        args: [],
        with_retries
    );
}

#[fasync::run_singlethreaded(test)]
async fn test_list_no_duplicates() {
    let (_env, _app) = utils::start_test_component("list-dup-test").await.expect("create comp 1");
    assert_command!(
        command: "list",
        golden_basename: list_no_dups,
        args: [],
        with_retries
    );
}

// List files command

#[fasync::run_singlethreaded(test)]
async fn list_files_empty_path_uses_cwd() {
    std::env::set_current_dir(Path::new("/hub")).expect("change dir");
    let (_env, _app) =
        utils::start_basic_component("list-file-test-1").await.expect("create comp 1");
    assert_command!(
        command: "list-files",
        golden_basename: list_files_cwd,
        args: []
    );
}

#[fasync::run_singlethreaded(test)]
async fn list_files() {
    let (_env, _app) =
        utils::start_basic_component("list-file-test-2").await.expect("create comp 1");
    let (_env2, _app2) =
        utils::start_test_component("list-file-test-3").await.expect("create comp 2");
    assert_command!(
        command: "list-files",
        golden_basename: list_files_test,
        args: [
            "/hub/c/observer.cmx/",
            "/hub/r/list-file-test-*/*/c/*/*/out/diagnostics/"
        ]
    );
}

// Selectors command

#[fasync::run_singlethreaded(test)]
async fn test_selectors_empty() {
    let result = utils::execute_command(&["selectors"]).await;
    assert_matches!(result, Err(Error::InvalidArguments(_)));
}

#[fasync::run_singlethreaded(test)]
async fn test_selectors() {
    let (_env, _app) = utils::start_basic_component("selectors-test").await.expect("create comp 1");
    let (_env2, _app2) =
        utils::start_basic_component("selectors-test2").await.expect("create comp 2");
    let (_env3, _app3) =
        utils::start_test_component("selectors-test3").await.expect("create comp 3");
    assert_command!(
        command: "selectors",
        golden_basename: selectors_test,
        args: [
            "selectors-test/basic_component.cmx:root/fuchsia.inspect.Health",
            "selectors-test2/basic_component.cmx:root",
            "selectors-test3/test_component.cmx"
        ],
        with_retries
    );
}

// Show file

#[fasync::run_singlethreaded(test)]
async fn test_no_paths() {
    let result = utils::execute_command(&["show-file"]).await;
    assert_matches!(result, Err(Error::InvalidArguments(_)));
}

#[fasync::run_singlethreaded(test)]
async fn test_invalid_location() {
    let dir = tempdir().unwrap();
    let file_path = dir.path().join("root.inspect").to_string_lossy().to_string();
    let result = utils::execute_command(&["show-file", &file_path]).await;
    assert_matches!(result, Err(Error::ReadLocation(path, _)) if path == file_path);
}

#[fasync::run_singlethreaded(test)]
async fn show_file_test() {
    let (_env, _app) =
        utils::start_basic_component("show-file-test-1").await.expect("create comp 1");
    let (_env2, _app2) =
        utils::start_test_component("show-file-test-2").await.expect("create comp 2");
    assert_command!(
        command: "show-file",
        golden_basename: show_file_test,
        args: [
            "/hub/r/show-file-test-1/*/c/basic_component.cmx/*/out/diagnostics/fuchsia.inspect.Tree",
            "/hub/r/show-file-test-2/*/c/test_component.cmx/*/out/diagnostics/*"
        ]
    );
}

#[fasync::run_singlethreaded(test)]
async fn inspect_vmo_file_directly() {
    let (_env, _app) = utils::start_test_component("show-file-vmo-2").await.expect("create comp 2");
    let paths = expand_paths(&[
        "/hub/r/show-file-vmo-2/*/c/test_component.cmx/*/out/diagnostics/*".to_string(),
    ])
    .expect("got paths");

    // Pass only the path to the vmo file. Without the workaround in `get_paths` comments this
    // wouldn't work and the `result` would be an emtpy list.
    let path =
        paths.into_iter().find(|p| p.ends_with("root.inspect")).expect("found root.inspect path");
    assert_command!(
        command: "show-file",
        golden_basename: show_file_vmo,
        args: [ &path ]
    );
}

// Show

#[fasync::run_singlethreaded(test)]
async fn test_no_selectors() {
    let result = utils::execute_command(&["show"]).await;
    assert_matches!(result, Err(Error::InvalidArguments(_)));
}

#[fasync::run_singlethreaded(test)]
async fn show_test() {
    let (_env, _app) = utils::start_basic_component("show-test").await.expect("create comp 1");
    let (_env2, _app2) = utils::start_basic_component("show-test2").await.expect("create comp 2");
    let (_env3, _app3) = utils::start_basic_component("show-test3").await.expect("create comp 3");
    assert_command!(
        command: "show",
        golden_basename: show_test,
        args: [
            "show-test/basic_component.cmx:root/fuchsia.inspect.Health",
            "show-test2/basic_component.cmx:root:iquery",
            "show-test3/basic_component.cmx"
        ],
        with_retries
    );
}

#[fasync::run_singlethreaded(test)]
async fn empty_result_on_null_payload() {
    let (_env, _app) =
        utils::start_basic_component("show-test-empty").await.expect("create comp 1");
    let result =
        utils::execute_command(&["show", "show-test-empty/basic_component.cmx:root/nothing:here"])
            .await;
    assert_matches!(result, Ok(res) if res == "");
}