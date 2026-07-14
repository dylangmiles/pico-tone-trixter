source "https://rubygems.org"

# The site is built by GitHub Actions (.github/workflows/pages.yml) using this
# exact gem set, so a local `bundle exec jekyll serve` matches what gets published.
gem "jekyll", "~> 4.4"
gem "minima", "~> 2.5"

group :jekyll_plugins do
  gem "jekyll-feed"
  gem "jekyll-seo-tag"
  gem "jekyll-sitemap"
end

# Ruby 3+ no longer ships webrick as a default gem; `jekyll serve` needs it.
gem "webrick"