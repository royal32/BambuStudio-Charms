// Account Switcher UI for BambuStudio Home Page
// This script is injected into the home page to provide multi-account functionality

(function() {
    'use strict';
    
    let accounts = [];
    let accountSwitcherVisible = false;
    
    // Create account switcher UI
    function createAccountSwitcher() {
        // Check if already exists
        if (document.getElementById('account-switcher-container')) {
            return;
        }
        
        const container = document.createElement('div');
        container.id = 'account-switcher-container';
        container.style.cssText = `
            position: fixed;
            top: 20px;
            right: 20px;
            z-index: 10000;
            font-family: Arial, sans-serif;
        `;
        
        const toggleButton = document.createElement('button');
        toggleButton.id = 'account-switcher-toggle';
        toggleButton.innerHTML = '👤 Accounts';
        toggleButton.style.cssText = `
            background: #00AE42;
            color: white;
            border: none;
            border-radius: 20px;
            padding: 10px 20px;
            cursor: pointer;
            font-size: 14px;
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.2);
        `;
        toggleButton.onclick = toggleAccountList;
        
        const dropdown = document.createElement('div');
        dropdown.id = 'account-dropdown';
        dropdown.style.cssText = `
            display: none;
            position: absolute;
            top: 45px;
            right: 0;
            background: white;
            border-radius: 8px;
            box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
            min-width: 280px;
            max-height: 400px;
            overflow-y: auto;
        `;
        
        container.appendChild(toggleButton);
        container.appendChild(dropdown);
        document.body.appendChild(container);
        
        // Request account list from the app
        requestAccountList();
    }
    
    function toggleAccountList() {
        const dropdown = document.getElementById('account-dropdown');
        if (dropdown.style.display === 'none') {
            dropdown.style.display = 'block';
            accountSwitcherVisible = true;
        } else {
            dropdown.style.display = 'none';
            accountSwitcherVisible = false;
        }
    }
    
    function requestAccountList() {
        // Send message to C++ to get account list
        const message = {
            sequence_id: '10002',
            command: 'get_account_list'
        };
        window.postMessage(JSON.stringify(message), '*');
    }
    
    function updateAccountList(accountsData) {
        accounts = accountsData;
        const dropdown = document.getElementById('account-dropdown');
        if (!dropdown) return;
        
        dropdown.innerHTML = '';
        
        if (accounts.length === 0) {
            dropdown.innerHTML = '<div style="padding: 20px; text-align: center; color: #666;">No accounts added yet</div>';
            return;
        }
        
        accounts.forEach(account => {
            const accountItem = document.createElement('div');
            accountItem.style.cssText = `
                padding: 12px 16px;
                border-bottom: 1px solid #f0f0f0;
                cursor: pointer;
                display: flex;
                align-items: center;
                justify-content: space-between;
                transition: background 0.2s;
            `;
            
            accountItem.onmouseover = function() {
                if (!account.is_active) {
                    this.style.background = '#f5f5f5';
                }
            };
            accountItem.onmouseout = function() {
                if (!account.is_active) {
                    this.style.background = '';
                }
            };
            
            if (account.is_active) {
                accountItem.style.background = '#e8f5e9';
            }
            
            const accountInfo = document.createElement('div');
            accountInfo.style.cssText = 'flex: 1;';
            
            const accountName = document.createElement('div');
            accountName.textContent = account.nickname || account.user_name || account.user_id;
            accountName.style.cssText = `
                font-weight: ${account.is_active ? 'bold' : 'normal'};
                margin-bottom: 4px;
                color: #333;
            `;
            
            const accountId = document.createElement('div');
            accountId.textContent = account.user_id;
            accountId.style.cssText = 'font-size: 12px; color: #666;';
            
            accountInfo.appendChild(accountName);
            accountInfo.appendChild(accountId);
            
            const actions = document.createElement('div');
            actions.style.cssText = 'display: flex; gap: 8px;';
            
            if (account.is_active) {
                const activeLabel = document.createElement('span');
                activeLabel.textContent = '✓ Active';
                activeLabel.style.cssText = 'color: #00AE42; font-size: 12px; font-weight: bold;';
                actions.appendChild(activeLabel);
            } else {
                const switchButton = document.createElement('button');
                switchButton.textContent = 'Switch';
                switchButton.style.cssText = `
                    background: #00AE42;
                    color: white;
                    border: none;
                    border-radius: 4px;
                    padding: 4px 12px;
                    cursor: pointer;
                    font-size: 12px;
                `;
                switchButton.onclick = function(e) {
                    e.stopPropagation();
                    switchAccount(account.user_id);
                };
                actions.appendChild(switchButton);
            }
            
            if (accounts.length > 1) {
                const removeButton = document.createElement('button');
                removeButton.textContent = '×';
                removeButton.style.cssText = `
                    background: #f44336;
                    color: white;
                    border: none;
                    border-radius: 4px;
                    padding: 4px 8px;
                    cursor: pointer;
                    font-size: 16px;
                    line-height: 12px;
                `;
                removeButton.onclick = function(e) {
                    e.stopPropagation();
                    if (confirm('Remove account ' + (account.nickname || account.user_name || account.user_id) + '?')) {
                        removeAccount(account.user_id);
                    }
                };
                actions.appendChild(removeButton);
            }
            
            accountItem.appendChild(accountInfo);
            accountItem.appendChild(actions);
            
            if (!account.is_active) {
                accountItem.onclick = function() {
                    switchAccount(account.user_id);
                };
            }
            
            dropdown.appendChild(accountItem);
        });
    }
    
    function switchAccount(userId) {
        const message = {
            sequence_id: '10003',
            command: 'switch_account',
            data: {
                user_id: userId
            }
        };
        window.postMessage(JSON.stringify(message), '*');
        
        // Close dropdown after switching
        setTimeout(() => {
            const dropdown = document.getElementById('account-dropdown');
            if (dropdown) {
                dropdown.style.display = 'none';
            }
        }, 100);
    }
    
    function removeAccount(userId) {
        const message = {
            sequence_id: '10004',
            command: 'remove_account',
            data: {
                user_id: userId
            }
        };
        window.postMessage(JSON.stringify(message), '*');
    }
    
    // Listen for messages from C++
    window.addEventListener('message', function(event) {
        try {
            let data;
            if (typeof event.data === 'string') {
                data = JSON.parse(event.data);
            } else {
                data = event.data;
            }
            
            if (data.command === 'account_list' && data.accounts) {
                updateAccountList(data.accounts);
            }
        } catch (e) {
            // Ignore parsing errors for non-JSON messages
        }
    });
    
    // Initialize account switcher when page loads
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', createAccountSwitcher);
    } else {
        createAccountSwitcher();
    }
    
    // Also try after a delay in case DOM isn't ready
    setTimeout(createAccountSwitcher, 1000);
    
})();
