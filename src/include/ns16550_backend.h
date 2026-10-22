#ifndef	NS16550_BACKEND_H
#define	NS16550_BACKEND_H

/*
 *  Host-side backends for dev_ns16550: a machine can route a UART's
 *  data and modem lines to something other than the GXemul console.
 *
 *  The machine installs a resolver that maps a UART's name (devinit name2)
 *  to its backend, or to NULL for the console. It is asked on every access,
 *  so a backend can come and go at run time.
 *
 *  Hooks marked "optional" may be NULL. The BE-300 serial dock behaviours
 *  in dev_ns16550.c are keyed on kind.
 */

#include <stddef.h>
#include <stdint.h>

enum ns16550_backend_kind {
	NS16550_BACKEND_PCCONNECT,	/*  BE-300 PC Connect dock cable  */
	NS16550_BACKEND_STOWAWAY,	/*  BE-300 Stowaway keyboard dock  */
	NS16550_BACKEND_SERIAL_BRIDGE	/*  plain host serial bridge  */
};

struct ns16550_backend {
	enum ns16550_backend_kind kind;

	size_t	(*rx_count)(const char *name);
	int	(*rx_pop)(const char *name);	/*  -1 if empty  */
	void	(*rx_clear)(const char *name);
	void	(*tx_byte)(const char *name, uint8_t byte);

	/*  PC Connect (optional):  */
	int	(*cable_connected)(void);
	uint32_t (*baud)(void);			/*  0: unknown  */
	void	(*signal_irq)(void);
	void	(*note_config)(const char *name, uint8_t lcr, uint8_t mcr,
		    uint8_t ier, int divisor, int dlab);
	int	(*trace_enabled)(void);
	void	(*trace_access)(const char *name, int write, unsigned reg,
		    uint8_t value, uint32_t pc, uint8_t ier, uint8_t iir,
		    uint8_t lcr, uint8_t mcr, uint8_t lsr, uint8_t msr,
		    int dlab, int divisor);

	/*  Stowaway (optional):  */
	int	(*take_modem_delta)(void);
	void	(*note_modem_wait)(void);
	void	(*note_port_config)(void);
	void	(*note_modem_control)(int dtr, int rts);
};

typedef const struct ns16550_backend *(*ns16550_backend_resolver_t)(
    const char *name);

void ns16550_set_backend_resolver(ns16550_backend_resolver_t resolver);

#endif	/*  NS16550_BACKEND_H  */
