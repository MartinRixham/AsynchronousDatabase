module.exports = function(grunt) {

	const port = 8910;

	grunt.loadNpmTasks('grunt-eslint');
	grunt.loadNpmTasks('grunt-contrib-connect');
	grunt.loadNpmTasks('grunt-contrib-qunit');

	grunt.initConfig({
		pkg: grunt.file.readJSON('package.json'),
		eslint: {
			target: ['js/**/*.js', 'test/**/*.js']
		},
		connect: {
			server: {
				options: {
					port: port,
					base: 'dist/test'
				}
			}
		},
		qunit: {
			all: {
				options: {
					urls: ['http://localhost:' + port],
					puppeteer: {
						executablePath: "chromium-browser"
					}
				}
			}
		}
	});

	grunt.registerTask('default', ['eslint', 'connect', 'qunit']);
};
