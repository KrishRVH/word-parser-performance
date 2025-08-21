#!/bin/bash
# install-deps.sh - Install all dependencies for the word count benchmark on WSL2/Ubuntu
# Works with both bash and zsh

echo "======================================"
echo "Installing Dependencies for Benchmark"
echo "======================================"
echo ""

# Detect the user's shell
USER_SHELL=$(basename "$SHELL")
if [ "$USER_SHELL" = "zsh" ]; then
    SHELL_RC="$HOME/.zshrc"
    echo "🐚 Detected shell: zsh"
else
    SHELL_RC="$HOME/.bashrc"
    echo "🐚 Detected shell: bash"
fi
echo "   Config file: $SHELL_RC"
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
if ! command -v node &> /dev/null; then
    curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
    sudo apt install -y nodejs
else
    echo "   Node.js already installed: $(node --version)"
fi

# Install PHP
echo ""
echo "🐘 Installing PHP..."
sudo apt install -y php-cli

# Install Rust
echo ""
echo "🦀 Installing Rust..."
if ! command -v rustc &> /dev/null; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
    
    # Add Rust to PATH for current session
    source "$HOME/.cargo/env"
    
    # Add Rust to appropriate shell config if not already there
    if ! grep -q ".cargo/env" "$SHELL_RC"; then
        echo 'source "$HOME/.cargo/env"' >> "$SHELL_RC"
        echo "   Added Rust to $SHELL_RC"
    fi
else
    echo "   Rust already installed: $(rustc --version)"
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
    
    # Add Go to PATH for current session
    export PATH=$PATH:/usr/local/go/bin
    
    # Add Go to appropriate shell config if not already there
    if ! grep -q "/usr/local/go/bin" "$SHELL_RC"; then
        echo 'export PATH=$PATH:/usr/local/go/bin' >> "$SHELL_RC"
        echo "   Added Go to $SHELL_RC"
    fi
else
    echo "   Go already installed: $(go version)"
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
    echo "   .NET already installed: $(dotnet --version)"
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
gcc --version | head -n1 || echo "GCC not found"
node --version 2>/dev/null || echo "Node.js not found"
php --version | head -n1 || echo "PHP not found"
rustc --version 2>/dev/null || echo "Rust not found - reload shell"
go version 2>/dev/null || echo "Go not found - reload shell"
dotnet --version 2>/dev/null || echo ".NET not found"
echo ""
echo "======================================"
echo "Next steps:"
echo "======================================"
echo ""
echo "1. Reload your shell configuration:"
if [ "$USER_SHELL" = "zsh" ]; then
    echo "   source ~/.zshrc"
else
    echo "   source ~/.bashrc"
fi
echo "   Or simply open a new terminal"
echo ""
echo "2. Make the benchmark executable:"
echo "   chmod +x bench.sh"
echo ""
echo "3. Run the benchmark:"
echo "   ./bench.sh"
echo ""
echo "That's it! The benchmark will handle everything else."