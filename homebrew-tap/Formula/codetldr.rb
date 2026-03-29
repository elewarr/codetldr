class Codetldr < Formula
  desc "Token-efficient code analysis for LLM agents"
  homepage "https://github.com/elewarr/codetldr"
  url "https://github.com/elewarr/codetldr/releases/download/v2.3.1/codetldr-2.3.1-darwin-arm64.tar.gz"
  sha256 "cedc03e95ea19fc975130f83e5e54821a85b75042bb96d4724167cebfc02ec2c"
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
