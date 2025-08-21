#!/bin/bash
# install-deps.sh - Install all dependencies for the word count benchmark on WSL2/Ubuntu

echo "======================================"
echo "Installing Dependencies for Benchmark"
echo "======================================"
echo ""

# Update package list
echo "📦 Updating package list..."
sudo apt update

# Install build essentials (includes gcc)
echo ""
echo "🔧 Installing build essentials (C compiler)..."
sudo apt install -y build-essential

# Install curl (for downloading test files)
echo ""
echo "🌐 Installing curl..."
sudo apt install -y curl

# Install bc (for benchmark calculations)
echo ""
echo "🧮 Installing bc (calculator)..."
sudo apt install -y bc

# Install Node.js (via NodeSource repository for latest version)
echo ""
echo "📗 Installing Node.js..."
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt install -y nodejs

# Install PHP
echo ""
echo "🐘 Installing PHP..."
sudo apt install -y php-cli

# Install Rust
echo ""
echo "🦀 Installing Rust..."
if ! command -v rustc &> /dev/null; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
    source "$HOME/.cargo/env"
else
    echo "   Rust already installed"
fi

# Install Go
echo ""
echo "🐹 Installing Go..."
if ! command -v go &> /dev/null; then
    GO_VERSION="1.21.6"
    wget -q https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz
    sudo rm -rf /usr/local/go
    sudo tar -C /usr/local -xzf go${GO_VERSION}.linux-amd64.tar.gz
    rm go${GO_VERSION}.linux-amd64.tar.gz
    
    # Add Go to PATH if not already there
    if ! grep -q "/usr/local/go/bin" ~/.bashrc; then
        echo 'export PATH=$PATH:/usr/local/go/bin' >> ~/.bashrc
    fi
    export PATH=$PATH:/usr/local/go/bin
else
    echo "   Go already installed"
fi

# Install .NET SDK
echo ""
echo "📘 Installing .NET SDK..."
if ! command -v dotnet &> /dev/null; then
    # Install Microsoft package repository
    wget -q https://packages.microsoft.com/config/ubuntu/$(lsb_release -rs)/packages-microsoft-prod.deb
    sudo dpkg -i packages-microsoft-prod.deb
    rm packages-microsoft-prod.deb
    
    # Install .NET SDK
    sudo apt update
    sudo apt install -y dotnet-sdk-8.0
else
    echo "   .NET already installed"
fi

# Install time command (for better benchmarking)
echo ""
echo "⏱️  Installing GNU time..."
sudo apt install -y time

echo ""
echo "======================================"
echo "✅ Installation Complete!"
echo "======================================"
echo ""
echo "Installed versions:"
echo "-------------------"
gcc --version | head -n1
node --version | head -n1
php --version | head -n1
rustc --version
go version
dotnet --version
echo ""
echo "To run the benchmark:"
echo "  1. Make sure all wordcount files are in the current directory"
echo "  2. Run: chmod +x bench.sh"
echo "  3. Run: ./bench.sh"
echo ""
echo "Note: If Rust or Go were just installed, run:"
echo "  source ~/.bashrc"
echo "Or open a new terminal for PATH updates to take effect."