#pragma once

char *vm_ssh_key_private_path(const char *dir);
char *vm_ssh_key_public_path(const char *dir);
void vm_ssh_key_ensure(const char *dir);
char *vm_ssh_key_public_line(const char *dir);
void vm_ssh_config_ensure(void);
