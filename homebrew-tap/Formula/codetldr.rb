class Codetldr < Formula
  desc "Token-efficient code analysis for LLM agents"
  homepage "https://github.com/codetldr/codetldr"
  url "https://github.com/codetldr/codetldr/releases/download/v0.1.0/codetldr-0.1.0-darwin-arm64.tar.gz"
  sha256 "PLACEHOLDER"
  license "MIT"

  def install
    bin.install "bin/codetldr"
    bin.install "bin/codetldr-daemon"
    bin.install "bin/codetldr-mcp"
  end

  def caveats
    <<~EOS
      To enable Swift support, install Xcode or Command Line Tools:
        xcode-select --install

      To set up CodeTLDR for a project:
        cd your-project && codetldr init
    EOS
  end

  test do
    system "#{bin}/codetldr", "--version"
    system "#{bin}/codetldr-mcp", "--version"
    system "#{bin}/codetldr-daemon", "--version"
  end
end
