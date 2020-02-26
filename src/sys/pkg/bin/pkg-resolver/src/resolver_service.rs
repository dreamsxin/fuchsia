// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::cache::{BlobFetcher, PackageCache},
    crate::font_package_manager::FontPackageManager,
    crate::queue,
    crate::repository_manager::RepositoryManager,
    crate::rewrite_manager::RewriteManager,
    anyhow::Error,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io::{self, DirectoryMarker},
    fidl_fuchsia_pkg::{
        FontResolverRequest, FontResolverRequestStream, PackageResolverRequest,
        PackageResolverRequestStream,
    },
    fidl_fuchsia_pkg_ext::BlobId,
    fuchsia_syslog::{fx_log_err, fx_log_info, fx_log_warn},
    fuchsia_trace as trace,
    fuchsia_url::pkg_url::{ParseError, PkgUrl},
    fuchsia_zircon::Status,
    futures::prelude::*,
    parking_lot::RwLock,
    std::sync::Arc,
    system_image::CachePackages,
};

pub type PackageFetcher = queue::WorkSender<PkgUrl, (), Result<BlobId, Status>>;

pub fn make_package_fetch_queue(
    cache: PackageCache,
    system_cache_list: Arc<CachePackages>,
    repo_manager: Arc<RwLock<RepositoryManager>>,
    blob_fetcher: BlobFetcher,
    max_concurrency: usize,
) -> (impl Future<Output = ()>, PackageFetcher) {
    let (package_fetch_queue, package_fetcher) =
        queue::work_queue(max_concurrency, move |url: PkgUrl, _: ()| {
            let cache = cache.clone();
            let system_cache_list = Arc::clone(&system_cache_list);
            let repo_manager = Arc::clone(&repo_manager);
            let blob_fetcher = blob_fetcher.clone();
            async move {
                let fut = repo_manager.read().get_package(
                    &url,
                    &cache,
                    &system_cache_list,
                    &blob_fetcher,
                );
                let merkle = fut.await?;
                fx_log_info!("resolved {} to {}", url, merkle);
                Ok(merkle)
            }
        });
    (package_fetch_queue.into_future(), package_fetcher)
}

pub async fn run_resolver_service(
    cache: PackageCache,
    repo_manager: Arc<RwLock<RepositoryManager>>,
    rewriter: Arc<RwLock<RewriteManager>>,
    package_fetcher: Arc<PackageFetcher>,
    stream: PackageResolverRequestStream,
) -> Result<(), Error> {
    stream
        .map_err(anyhow::Error::new)
        .try_for_each_concurrent(None, |event| {
            async {
                match event {
                    PackageResolverRequest::Resolve {
                        package_url,
                        selectors,
                        update_policy: _,
                        dir,
                        responder,
                    } => {
                        // FIXME: need to implement selectors.
                        if !selectors.is_empty() {
                            fx_log_warn!("resolve does not support selectors yet");
                        }
                        let status =
                            resolve(&rewriter, &cache, &package_fetcher, package_url, dir).await;
                        responder.send(Status::from(status).into_raw())?;
                        Ok(())
                    }
                    PackageResolverRequest::GetHash { package_url, responder } => {
                        match get_hash(&rewriter, &repo_manager, &package_url.url).await {
                            Ok(blob_id) => {
                                fx_log_info!("hash of {} is {}", package_url.url, blob_id);
                                responder.send(&mut Ok(blob_id.into()))?;
                            }
                            Err(status) => {
                                responder.send(&mut Err(status.into_raw()))?;
                            }
                        }
                        Ok(())
                    }
                }
            }
        })
        .await
}

fn rewrite_url(rewriter: &RwLock<RewriteManager>, pkg_url: &str) -> Result<PkgUrl, Status> {
    let url = match PkgUrl::parse(pkg_url) {
        Ok(url) => url,
        Err(err) => {
            return Err(handle_bad_package_url(err, pkg_url));
        }
    };
    let url = rewriter.read().rewrite(url);

    // While the fuchsia-pkg:// spec allows resource paths, the package resolver should not be
    // given one.
    if url.resource().is_some() {
        fx_log_err!("package url should not contain a resource name: {}", url);
        return Err(Status::INVALID_ARGS);
    }
    Ok(url)
}

async fn get_hash(
    rewriter: &RwLock<RewriteManager>,
    repo_manager: &RwLock<RepositoryManager>,
    pkg_url: &str,
) -> Result<BlobId, Status> {
    let url = rewrite_url(rewriter, pkg_url)?;
    trace::duration_begin!("app", "get-hash", "url" => pkg_url);
    let hash_or_status = repo_manager.read().get_package_hash(&url).await;
    trace::duration_end!("app", "get-hash", "status" => Status::from(hash_or_status.err().unwrap_or(Status::OK)).to_string().as_str());
    hash_or_status
}

/// Resolve the package.
///
/// FIXME: at the moment, we are proxying to Amber to resolve a package name and variant to a
/// merkleroot. Because of this, we cant' implement the update policy, so we just ignore it.
async fn resolve(
    rewriter: &RwLock<RewriteManager>,
    cache: &PackageCache,
    package_fetcher: &PackageFetcher,
    pkg_url: String,
    dir_request: ServerEnd<DirectoryMarker>,
) -> Result<(), Status> {
    let url = rewrite_url(rewriter, &pkg_url)?;
    trace::duration_begin!("app", "resolve", "url" => pkg_url.as_str());
    let queued_fetch = package_fetcher.push(url, ());
    let merkle_or_status = queued_fetch.await.expect("expected queue to be open");
    trace::duration_end!("app", "resolve", "status" => Status::from(merkle_or_status.err().unwrap_or(Status::OK)).to_string().as_str());
    match merkle_or_status {
        Ok(merkle) => {
            let selectors = vec![];
            cache
                .open(merkle, &selectors, dir_request)
                .await
                .map_err(|err| handle_bad_package_open(err, &pkg_url))
        }
        Err(status) => Err(status),
    }
}

/// Run a service that only resolves registered font packages.
pub async fn run_font_resolver_service(
    font_package_manager: Arc<FontPackageManager>,
    cache: PackageCache,
    rewrites: Arc<RwLock<RewriteManager>>,
    package_fetcher: Arc<PackageFetcher>,
    stream: FontResolverRequestStream,
) -> Result<(), Error> {
    stream
        .map_err(anyhow::Error::new)
        .try_for_each_concurrent(None, |event| async {
            let FontResolverRequest::Resolve {
                package_url,
                update_policy: _,
                directory_request,
                responder,
            } = event;

            let status = resolve_font(
                &font_package_manager,
                &rewrites,
                &cache,
                &package_fetcher,
                package_url,
                directory_request,
            )
            .await;
            responder.send(Status::from(status).into_raw())?;

            Ok(())
        })
        .await
}

/// Resolve a single font package.
async fn resolve_font<'a>(
    font_package_manager: &'a Arc<FontPackageManager>,
    rewrites: &'a Arc<RwLock<RewriteManager>>,
    cache: &'a PackageCache,
    package_fetcher: &'a Arc<PackageFetcher>,
    package_url: String,
    directory_request: ServerEnd<DirectoryMarker>,
) -> Result<(), Status>
where
{
    match PkgUrl::parse(&package_url) {
        Err(err) => Err(handle_bad_package_url(err, &package_url)),
        Ok(parsed_package_url) => {
            if !font_package_manager.is_font_package(&parsed_package_url) {
                fx_log_err!("tried to resolve unknown font package: {}", package_url);
                Err(Status::NOT_FOUND)
            } else {
                resolve(&rewrites, &cache, &package_fetcher, package_url, directory_request).await
            }
        }
    }
}

fn handle_bad_package_url(parse_error: ParseError, pkg_url: &str) -> Status {
    fx_log_err!("failed to parse package url {:?}: {}", pkg_url, parse_error);
    Status::INVALID_ARGS
}

fn handle_bad_package_open(open_error: crate::cache::PackageOpenError, pkg_url: &str) -> Status {
    fx_log_err!("failed to open package url {:?}: {}", pkg_url, open_error);
    Status::from(open_error)
}
